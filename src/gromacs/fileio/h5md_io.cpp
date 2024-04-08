/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright 2023- The GROMACS Authors
 * and the project initiators Erik Lindahl, Berk Hess and David van der Spoel.
 * Consult the AUTHORS/COPYING files and https://www.gromacs.org for details.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * https://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at https://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out https://www.gromacs.org.
 */

#include "gmxpre.h"

#include "h5md_io.h"

#include "config.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/baseversion.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/programcontext.h"
#include "gromacs/utility/sysinfo.h"

#include "h5md_datablock.h"
#include "h5md_util.h"

#define GMX_USE_HDF5 1 // FIXME: Temporary just for the editor

#if GMX_USE_HDF5
#    include <hdf5.h>

#    include "external/SZ3-bio/tools/H5Z-SZ3/include/H5Z_SZ3.hpp"
#endif

namespace
{

/*! \brief Iterates through groups with contents matching time dependent particles data blocks,
 * i.e., "step", "time" and "value". Then it creates corresponding H5MD data blocks.
 * Inspired by https://support.hdfgroup.org/ftp/HDF5/examples/examples-by-api/hdf5-examples/1_8/C/H5G/h5ex_g_traverse.c
 */
herr_t iterativeSetupTimeDataBlocks(hid_t            locationId,
                                    const char*      name,
                                    const H5L_info_t gmx_unused* info,
                                    void*                        operatorData)
{
    /*
     * Get type of the object. The name of the object is passed to this function by
     * the Library.
     */
    H5O_info_t infoBuffer;
    H5Oget_info_by_name(locationId, name, &infoBuffer, H5P_DEFAULT);
    herr_t            returnVal        = 0;
    const std::string stepDataSetName  = std::string(name) + std::string("/step");
    const std::string timeDataSetName  = std::string(name) + std::string("/time");
    const std::string valueDataSetName = std::string(name) + std::string("/value");
    switch (infoBuffer.type)
    {
        case H5O_TYPE_GROUP:
            if (gmx::h5mdio::objectExists(locationId, stepDataSetName.c_str())
                && gmx::h5mdio::objectExists(locationId, timeDataSetName.c_str())
                && gmx::h5mdio::objectExists(locationId, valueDataSetName.c_str()))
            {
                char containerFullName[gmx::h5mdio::c_maxFullNameLength];
                H5Iget_name(locationId, containerFullName, gmx::h5mdio::c_maxFullNameLength);
                gmx::h5mdio::GmxH5mdTimeDataBlock             dataBlock(locationId, name);
                std::list<gmx::h5mdio::GmxH5mdTimeDataBlock>* dataBlocks =
                        static_cast<std::list<gmx::h5mdio::GmxH5mdTimeDataBlock>*>(operatorData);

                dataBlock.updateNumWrittenFrames();
                dataBlocks->emplace_back(dataBlock);

                returnVal = 0;
            }
            else
            {
                returnVal = H5Literate_by_name(locationId,
                                               name,
                                               H5_INDEX_NAME,
                                               H5_ITER_NATIVE,
                                               nullptr,
                                               iterativeSetupTimeDataBlocks,
                                               operatorData,
                                               H5P_DEFAULT);
            }
            break;
        default: /* Ignore other contents */ break;
    }
    return returnVal;
}

/* Unused. May be useful later. */
/*
herr_t getGroupNamesInLocation(hid_t location, const char* name, const H5O_info_t* info, void* operatorData)
{
    if(info->type == H5O_TYPE_GROUP)
    {
        auto vec = static_cast<std::vector<std::string>*>(operatorData);
        vec->push_back(std::string(name));
    }
}
*/

} // namespace

namespace gmx
{
namespace h5mdio
{

GmxH5mdIo::GmxH5mdIo(const std::string fileName, const char mode)
{
    file_ = -1;
    if (fileName.length() > 0)
    {
        openFile(fileName.c_str(), mode);
    }
}

GmxH5mdIo::~GmxH5mdIo()
{
    if (file_ != -1)
    {
        closeFile();
    }
}

void GmxH5mdIo::openFile(const std::string fileName, const char mode)
{
#if GMX_USE_HDF5
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr); // Disable HDF5 error output, e.g. when items are not found.

    closeFile();

    dataBlocks_.clear();

    if (debug)
    {
        fprintf(debug, "Opening H5MD file %s with mode %c\n", fileName.c_str(), mode);
    }
    if (mode == 'w' || mode == 'a')
    {
        bool fileExists = gmx_fexist(fileName);
        if (!fileExists || mode == 'w')
        {
            make_backup(fileName.c_str());
            hid_t createPropertyList = H5Pcreate(H5P_FILE_CREATE);
            file_ = H5Fcreate(fileName.c_str(), H5F_ACC_TRUNC, createPropertyList, H5P_DEFAULT);
            if (file_ < 0)
            {
                throw gmx::FileIOError("Cannot create H5MD file.");
            }
        }
        else
        {
            file_ = H5Fopen(fileName.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        }
        /* Create H5MD groups. They should already be there if appending to a valid H5MD file, but it's better to be on the safe side. */
        openOrCreateGroup(file_, "h5md");
    }
    else
    {
        file_ = H5Fopen(fileName.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    }
    initGroupTimeDataBlocksFromFile("particles");
    initGroupTimeDataBlocksFromFile("observables");
    if (file_ < 0)
    {
        throw gmx::FileIOError("Cannot open H5MD file.");
    }
#else
    throw gmx::FileIOError(
            "GROMACS was compiled without HDF5 support, cannot handle this file type");
#endif
}

void GmxH5mdIo::closeFile()
{
#if GMX_USE_HDF5
    if (file_ >= 0)
    {
        if (H5Fflush(file_, H5F_SCOPE_LOCAL) < 0)
        {
            H5Eprint2(H5E_DEFAULT, nullptr);
            throw gmx::FileIOError("Error flushing H5MD file when closing.");
        }
        if (debug)
        {
            fprintf(debug, "Closing H5MD file.\n");
        }
        for (auto dataBlock : dataBlocks_)
        {
            dataBlock.closeAllDataSets();
        }
        H5Fclose(file_);
        file_ = -1;
    }
#else
    throw gmx::FileIOError(
            "GROMACS was compiled without HDF5 support, cannot handle this file type");
#endif
}

void GmxH5mdIo::flush()
{
#if GMX_USE_HDF5
    if (file_ >= 0)
    {
        if (debug)
        {
            fprintf(debug, "Flushing H5MD file.\n");
        }
        if (H5Fflush(file_, H5F_SCOPE_LOCAL) < 0)
        {
            H5Eprint2(H5E_DEFAULT, nullptr);
            throw gmx::FileIOError("Error flushing H5MD file when closing.");
        }
    }
#else
    throw gmx::FileIOError(
            "GROMACS was compiled without HDF5 support, cannot handle this file type");
#endif
}

int GmxH5mdIo::initGroupTimeDataBlocksFromFile(std::string groupName)
{
    int   numDataBlocksBefore = dataBlocks_.size();
    hid_t group               = H5Gopen(file_, groupName.c_str(), H5P_DEFAULT);
    if (group < 0)
    {
        if (debug)
        {
            fprintf(debug,
                    "Cannot find group %s when initializing particles data blocks. Invalid file?",
                    groupName.c_str());
        }
        return 0;
    }
    if (H5Literate(group,
                   H5_INDEX_NAME,
                   H5_ITER_NATIVE,
                   nullptr,
                   iterativeSetupTimeDataBlocks,
                   static_cast<void*>(&dataBlocks_))
        < 0)
    {
        H5Eprint2(H5E_DEFAULT, nullptr);
        throw gmx::FileIOError("Error iterating over particles data blocks.");
    }
    return dataBlocks_.size() - numDataBlocksBefore;
}

void GmxH5mdIo::setAuthor(std::string authorName)
{
    hid_t authorGroup = openOrCreateGroup(file_, "h5md/author");
    setAttribute(authorGroup, "name", authorName.c_str());
}

std::string GmxH5mdIo::getAuthor()
{
    hid_t authorGroup = openOrCreateGroup(file_, "h5md/author");
    char* tmpName     = nullptr;
    getAttribute(authorGroup, "name", &tmpName);
    std::string name(tmpName);
    H5free_memory(tmpName);
    return name;
}

void GmxH5mdIo::setCreatorProgramName(std::string creatorName)
{
    hid_t creatorGroup = openOrCreateGroup(file_, "h5md/creator");
    setAttribute(creatorGroup, "name", creatorName.c_str());
}

std::string GmxH5mdIo::getCreatorProgramName()
{
    hid_t creatorGroup = openOrCreateGroup(file_, "h5md/creator");
    char* tmpName      = nullptr;
    getAttribute(creatorGroup, "name", &tmpName);
    std::string name(tmpName);
    H5free_memory(tmpName);
    return name;
}

void GmxH5mdIo::setCreatorProgramVersion(std::string version)
{
    hid_t creatorGroup = openOrCreateGroup(file_, "h5md/creator");
    setAttribute(creatorGroup, "version", version.c_str());
}

std::string GmxH5mdIo::getCreatorProgramVersion()
{
    hid_t creatorGroup = openOrCreateGroup(file_, "h5md/creator");
    char* tmpVersion   = nullptr;
    getAttribute(creatorGroup, "version", &tmpVersion);
    std::string version(tmpVersion);
    H5free_memory(tmpVersion);
    return version;
}

void GmxH5mdIo::setStringProperty(const std::string&              containerName,
                                  const std::string&              propertyName,
                                  const std::vector<std::string>& propertyValues,
                                  bool                            replaceExisting)
{
    openOrCreateGroup(file_, containerName.c_str());
    std::string dataSetName(containerName + "/" + propertyName);

    if (!H5Lexists(file_, dataSetName.c_str(), H5P_DEFAULT) || replaceExisting == true)
    {
        /* FIXME: Is there a more convenient way to do this? std::string is nice above, but cannot be used for writing in HDF5. */
        char* propertyValuesChars;
        snew(propertyValuesChars, propertyValues.size() * c_atomStringLen);
        for (size_t i = 0; i < propertyValues.size(); i++)
        {
            strncpy(&propertyValuesChars[i * c_atomStringLen], propertyValues[i].c_str(), c_atomStringLen);
        }

        hid_t stringDataType = H5Tcopy(H5T_C_S1);
        H5Tset_size(stringDataType, c_atomStringLen);
        H5Tset_cset(stringDataType, H5T_CSET_UTF8);

        hsize_t atomPropertiesChunkDims[1];
        atomPropertiesChunkDims[0] = propertyValues.size();

        hid_t dataSet = openOrCreateDataSet<1>(file_,
                                               dataSetName.c_str(),
                                               "",
                                               stringDataType,
                                               atomPropertiesChunkDims,
                                               CompressionAlgorithm::LosslessNoShuffle,
                                               0);
        writeData<1, true>(dataSet, propertyValuesChars, 0);
        H5Dclose(dataSet);
        sfree(propertyValuesChars);
    }
}

void GmxH5mdIo::setFloatProperty(const std::string&       containerName,
                                 const std::string&       propertyName,
                                 const std::vector<real>& propertyValues,
                                 bool                     replaceExisting)
{
    openOrCreateGroup(file_, containerName.c_str());
    std::string dataSetName(containerName + "/" + propertyName);

    if (!H5Lexists(file_, dataSetName.c_str(), H5P_DEFAULT) || replaceExisting == true)
    {
#if GMX_DOUBLE
        const hid_t floatDatatype = H5Tcopy(H5T_NATIVE_DOUBLE);
#else
        const hid_t floatDatatype = H5Tcopy(H5T_NATIVE_FLOAT);
#endif

        hsize_t atomPropertiesChunkDims[1];
        atomPropertiesChunkDims[0] = propertyValues.size();

        hid_t dataSet = openOrCreateDataSet<1>(file_,
                                               dataSetName.c_str(),
                                               "",
                                               floatDatatype,
                                               atomPropertiesChunkDims,
                                               CompressionAlgorithm::LosslessNoShuffle,
                                               0);
        writeData<1, true>(dataSet, propertyValues.data(), 0);
        H5Dclose(dataSet);
    }
}

std::vector<std::string> GmxH5mdIo::readStringProperty(const std::string& containerName,
                                                       const std::string& propertyName)
{
    std::string              dataSetName(containerName + "/" + propertyName);
    hid_t                    dataSet = H5Dopen(file_, dataSetName.c_str(), H5P_DEFAULT);
    std::vector<std::string> propertyValues;

    if (dataSet < 0)
    {
        return propertyValues;
    }

    hsize_t stringDataTypeSize = c_atomStringLen;
    size_t  totalNumElements;

    char* propertyValuesChars = nullptr;
    readData<1, true>(
            dataSet, 0, stringDataTypeSize, reinterpret_cast<void**>(&propertyValuesChars), &totalNumElements);
    propertyValues.reserve(totalNumElements);

    for (size_t i = 0; i < totalNumElements; i++)
    {
        propertyValues.push_back(propertyValuesChars + i * c_atomStringLen);
    }

    H5free_memory(propertyValuesChars);
    return propertyValues;
}

std::vector<real> GmxH5mdIo::readFloatProperty(const std::string& containerName, const std::string& propertyName)
{
    std::string       dataSetName(containerName + "/" + propertyName);
    hid_t             dataSet = H5Dopen(file_, dataSetName.c_str(), H5P_DEFAULT);
    std::vector<real> propertyValues;
    printf("%s\n", dataSetName.c_str());

    if (dataSet < 0)
    {
        return propertyValues;
    }

    size_t totalNumElements;
    void*  buffer       = nullptr;
    size_t dataTypeSize = getDataTypeSize(dataSet);
    readData<1, true>(dataSet, 0, dataTypeSize, &buffer, &totalNumElements);
    propertyValues.reserve(totalNumElements);

    if (dataTypeSize == 8)
    {
        for (size_t i = 0; i < totalNumElements; i++)
        {
            propertyValues.push_back(static_cast<double*>(buffer)[i]);
        }
    }
    else
    {
        for (size_t i = 0; i < totalNumElements; i++)
        {
            propertyValues.push_back(static_cast<float*>(buffer)[i]);
        }
    }

    H5free_memory(buffer);

    return propertyValues;
}

void GmxH5mdIo::writeDataFrame(int64_t              step,
                               real                 time,
                               std::string          dataBlockFullName,
                               int                  dataDimensionalityFirstDim,
                               int                  dataDimensionalitySecondDim,
                               const real*          data,
                               std::string          unit,
                               hsize_t              numberOfFramesPerChunk,
                               CompressionAlgorithm compressionAlgorithm,
                               double               lossyCompressionError)

{
    GMX_ASSERT(data != nullptr, "Needs valid data to write a data frame.");
    GMX_ASSERT(dataDimensionalityFirstDim > 0 && dataDimensionalitySecondDim > 0,
               "The data dimensionality must be at least 1 in both dimensions.");
    /* See if the data block (API container for time dependent data sets) exists, otherwise create it */
    auto foundDataBlock = std::find(dataBlocks_.begin(), dataBlocks_.end(), dataBlockFullName.c_str());
    if (foundDataBlock == dataBlocks_.end())
    {
        std::size_t lastSeparatorPos = dataBlockFullName.find_last_of("/");
        std::string groupName        = dataBlockFullName.substr(0, lastSeparatorPos);
        std::string dataBlockName =
                dataBlockFullName.substr(lastSeparatorPos + 1, dataBlockFullName.length());
        hid_t group = openOrCreateGroup(file_, groupName.c_str());

#if GMX_DOUBLE
        const hid_t datatype = H5Tcopy(H5T_NATIVE_DOUBLE);
#else
        const hid_t datatype      = H5Tcopy(H5T_NATIVE_FLOAT);
#endif

        GmxH5mdTimeDataBlock dataBlock(group,
                                       dataBlockName,
                                       unit,
                                       numberOfFramesPerChunk,
                                       dataDimensionalityFirstDim,
                                       dataDimensionalitySecondDim,
                                       datatype,
                                       compressionAlgorithm,
                                       lossyCompressionError);
        dataBlocks_.emplace_back(dataBlock);
        foundDataBlock = std::find(dataBlocks_.begin(), dataBlocks_.end(), dataBlockFullName.c_str());
        if (foundDataBlock == dataBlocks_.end())
        {
            throw gmx::FileIOError("Error creating data block when writing frame.");
        }
        H5Gclose(group);
    }
    foundDataBlock->writeFrame(data, step, time);
}

bool GmxH5mdIo::readNextFrameOfDataBlock(std::string dataBlockFullName, real* data, int64_t stepToRead)
{
    for (auto& dataBlock : dataBlocks_)
    {
        if (dataBlock.fullName() == dataBlockFullName)
        {
            if (stepToRead < 0 || dataBlock.getStepOfNextReadingFrame() == stepToRead)
            {
                return dataBlock.readNextFrame(data);
            }
            return false;
        }
    }
    return false;
}

real GmxH5mdIo::getLossyCompressionErrorOfDataBlock(std::string dataBlockFullName)
{
    for (const auto& dataBlock : dataBlocks_)
    {
        if (dataBlock.fullName() == dataBlockFullName)
        {
            return dataBlock.getLossyCompressionError();
        }
    }
    return -1;
}

int64_t GmxH5mdIo::getNumberOfFrames(const std::string dataBlockName, std::string selectionName)
{
    GMX_ASSERT(dataBlockName != "", "There must be a datablock name to look for.");

    std::string wantedName = "/particles/" + selectionName + "/" + dataBlockName;

    auto foundDataBlock = std::find(dataBlocks_.begin(), dataBlocks_.end(), wantedName.c_str());
    if (foundDataBlock == dataBlocks_.end())
    {
        return -1;
    }
    return foundDataBlock->numberOfFrames();
}

int64_t GmxH5mdIo::getNumberOfParticles(const std::string dataBlockName, std::string selectionName)
{
    GMX_ASSERT(dataBlockName != "", "There must be a datablock name to look for.");

    std::string wantedName = "/particles/" + selectionName + "/" + dataBlockName;

    auto foundDataBlock = std::find(dataBlocks_.begin(), dataBlocks_.end(), wantedName.c_str());
    if (foundDataBlock == dataBlocks_.end())
    {
        return -1;
    }
    return foundDataBlock->getNumParticles();
}

real GmxH5mdIo::getFirstTime(const std::string dataBlockName, std::string selectionName)
{
    GMX_ASSERT(dataBlockName != "", "There must be a datablock name to look for.");

    std::string wantedName = "/particles/" + selectionName + "/" + dataBlockName;

    auto foundDataBlock = std::find(dataBlocks_.begin(), dataBlocks_.end(), wantedName.c_str());
    if (foundDataBlock == dataBlocks_.end())
    {
        return -1;
    }
    return foundDataBlock->getTimeOfFrame(0);
}

real GmxH5mdIo::getFirstTimeFromAllDataBlocks()
{
    real firstTime = std::numeric_limits<real>::max();
    bool foundAny  = false;
    for (const auto& dataBlock : dataBlocks_)
    {
        foundAny  = true;
        real time = dataBlock.getTimeOfFrame(0);
        firstTime = std::min(firstTime, time);
    }
    if (foundAny)
    {
        return firstTime;
    }
    return -1;
}

std::tuple<int64_t, real> GmxH5mdIo::getNextStepAndTimeToRead()
{
    int64_t minStepNextFrame = std::numeric_limits<int64_t>::max();
    real    minTime          = std::numeric_limits<real>::max();
    for (const auto& dataBlock : dataBlocks_)
    {
        int64_t frameStep = dataBlock.getStepOfNextReadingFrame();
        /* Discard data sets that had a higher time stamp if an earlier data point has been found. */
        if (frameStep >= 0 && frameStep < minStepNextFrame)
        {
            minStepNextFrame = frameStep;
            minTime          = dataBlock.getTimeOfFrame(dataBlock.readingFrameIndex());
        }
    }
    return std::tuple<int64_t, real>(minStepNextFrame, minTime);
}

real GmxH5mdIo::getFinalTime(const std::string dataBlockName, std::string selectionName)
{
    GMX_ASSERT(dataBlockName != "", "There must be a datablock name to look for.");

    std::string wantedName = "/particles/" + selectionName + "/" + dataBlockName;

    auto foundDataBlock = std::find(dataBlocks_.begin(), dataBlocks_.end(), wantedName.c_str());
    if (foundDataBlock == dataBlocks_.end())
    {
        return -1;
    }
    return foundDataBlock->getTimeOfFrame(foundDataBlock->numberOfFrames() - 1);
}

real GmxH5mdIo::getFinalTimeFromAllDataBlocks()
{
    real finalTime = 0;
    bool foundAny  = false;
    for (auto& dataBlock : dataBlocks_)
    {
        int64_t numFrames = dataBlock.numberOfFrames();
        if (numFrames < 1)
        {
            continue;
        }
        foundAny  = true;
        real time = dataBlock.getTimeOfFrame(numFrames - 1);
        finalTime = std::max(finalTime, time);
    }
    if (foundAny)
    {
        return finalTime;
    }
    return -1;
}


extern template hid_t
openOrCreateDataSet<1>(hid_t, const char*, const char*, hid_t, const hsize_t*, CompressionAlgorithm, double);
extern template hid_t
openOrCreateDataSet<2>(hid_t, const char*, const char*, hid_t, const hsize_t*, CompressionAlgorithm, double);

extern template void writeData<1, true>(hid_t, const void*, hsize_t);

extern template void readData<1, true>(hid_t, hsize_t, size_t, void**, size_t*);

extern template void setAttribute<int>(hid_t, const char*, int, hid_t);
extern template void setAttribute<float>(hid_t, const char*, float, hid_t);
extern template void setAttribute<double>(hid_t, const char*, double, hid_t);
extern template void setAttribute<char*>(hid_t, const char*, char*, hid_t);

} // namespace h5mdio

void setH5mdAuthorAndCreator(h5mdio::GmxH5mdIo* file)
{
    char tmpUserName[gmx::h5mdio::c_maxFullNameLength];
    if (!gmx_getusername(tmpUserName, gmx::h5mdio::c_maxFullNameLength))
    {
        file->setAuthor(tmpUserName);
    }

    std::string precisionString = "";
#if GMX_DOUBLE
    precisionString = " (double precision)";
#endif
    std::string programInfo = gmx::getProgramContext().displayName() + precisionString;
    file->setCreatorProgramName(programInfo);

    const std::string gmxVersion = gmx_version();
    file->setCreatorProgramVersion(gmxVersion);
}

void setupMolecularSystem(h5mdio::GmxH5mdIo*       file,
                          const gmx_mtop_t&        topology,
                          gmx::ArrayRef<const int> index,
                          std::string              selectionName)
{
#if GMX_USE_HDF5
    if (file == nullptr || !file->isFileOpen())
    {
        throw gmx::FileIOError("No file open for writing.");
    }

    t_atoms atoms = gmx_mtop_global_atoms(topology);

    if (atoms.nr == 0)
    {
        return;
    }

    /* Vectors are used to keep the values in a continuous memory block. */
    std::vector<real>        atomCharges;
    std::vector<real>        atomMasses;
    std::vector<std::string> atomNames;

    atomCharges.reserve(atoms.nr);
    atomMasses.reserve(atoms.nr);
    atomNames.reserve(atoms.nr);

    /* FIXME: The names could be copied directly to a char array instead. */
    /* FIXME: Should use int64_t. Needs changes in atoms. */
    for (int atomCounter = 0; atomCounter < atoms.nr; atomCounter++)
    {
        atomCharges.push_back(atoms.atom[atomCounter].q);
        atomMasses.push_back(atoms.atom[atomCounter].m);
        atomNames.push_back(*(atoms.atomname[atomCounter]));
    }

    file->setStringProperty("/particles/system", "atomname", atomNames, false);
    file->setFloatProperty("/particles/system", "atomname", atomCharges, false);
    file->setFloatProperty("/particles/system", "atomname", atomMasses, false);

    /* We only need to create a separate selection group entry if not all atoms are part of it. */
    /* TODO: Write atom name, charge and mass for the selection group as well. */
    /* If a selection of atoms is explicitly provided then use that instead of the CompressedPositionOutput */
    bool all_atoms_selected = true;
    if (index.ssize() > 0 && index.ssize() != topology.natoms)
    {
        all_atoms_selected = false;
    }
    else
    {
        /* FIXME: Should use int64_t. Needs changes in topology. */
        for (int i = 0; i < topology.natoms; i++)
        {
            if (getGroupType(topology.groups, SimulationAtomGroupType::CompressedPositionOutput, i) != 0)
            {
                all_atoms_selected = false;
                break;
            }
        }
    }
    if (!all_atoms_selected)
    {
        std::string systemOutputName;
        if (index.ssize() > 0 && selectionName != "")
        {
            systemOutputName = selectionName;
        }
        /* If no name was specified fall back to using the selection group name of compressed output, if any. */
        else if (topology.groups.numberOfGroupNumbers(SimulationAtomGroupType::CompressedPositionOutput) != 0)
        {
            int nameIndex = topology.groups.groups[SimulationAtomGroupType::CompressedPositionOutput][0];
            systemOutputName = *topology.groups.groupNames[nameIndex];
        }
        atomCharges.clear();
        atomMasses.clear();
        atomNames.clear();
        atomCharges.reserve(index.ssize());
        atomMasses.reserve(index.ssize());
        atomNames.reserve(index.ssize());
        for (int i = 0; i < index.ssize(); i++)
        {
            atomCharges.push_back(atoms.atom[i].q);
            atomMasses.push_back(atoms.atom[i].m);
            atomNames.push_back(*(atoms.atomname[i]));
        }

        file->setStringProperty("particles/" + systemOutputName, "atomname", atomNames, false);
        file->setFloatProperty("particles/" + systemOutputName, "charge", atomCharges, false);
        file->setFloatProperty("particles/" + systemOutputName, "mass", atomMasses, false);
    }

    done_atom(&atoms);

#else
    throw gmx::FileIOError(
            "GROMACS was compiled without HDF5 support, cannot handle this file type");
#endif
}

void writeFrameToStandardDataBlocks(h5mdio::GmxH5mdIo* file,
                                    int64_t            step,
                                    real               time,
                                    real               lambda,
                                    const rvec*        box,
                                    const int64_t      numParticles,
                                    const rvec*        x,
                                    const rvec*        v,
                                    const rvec*        f,
                                    const double       xCompressionError,
                                    const std::string  selectionName)
{
#if GMX_USE_HDF5
    if (numParticles <= 0)
    {
        throw gmx::FileIOError("There must be particles/atoms when writing trajectory frames.");
    }
    if (file == nullptr || !file->isFileOpen())
    {
        throw gmx::FileIOError("No file open for writing.");
    }

    /* There is so little lambda data per frame that it is best to write multiple per chunk. */
    hsize_t     numFramesPerChunk = 20;
    std::string wantedName        = "/observables/lambda";
    file->writeDataFrame(
            step, time, wantedName, 1, 1, &lambda, "", numFramesPerChunk, h5mdio::CompressionAlgorithm::LosslessNoShuffle);

    if (x != nullptr)
    {
        wantedName = "/particles/" + selectionName + "/position";
        h5mdio::CompressionAlgorithm compressionAlgorithm =
                h5mdio::CompressionAlgorithm::LosslessWithShuffle;
        if (xCompressionError != 0)
        {
            /* Use no more than 20 frames per chunk (compression unit). Use fewer frames per chunk if there are many atoms. */
            numFramesPerChunk    = std::min(20, int(std::ceil(5e6f / numParticles)));
            compressionAlgorithm = h5mdio::CompressionAlgorithm::LossySz3;

            /* Register the SZ3 filter. This is not necessary when creating a dataset with the filter,
             * but must be done to append to an existing file (e.g. when restarting from checkpoint). */
            h5mdio::registerSz3FilterImplicitly();
        }
        file->writeDataFrame(step,
                             time,
                             wantedName,
                             numParticles,
                             DIM,
                             static_cast<const real*>(x[0]),
                             "nm",
                             numFramesPerChunk,
                             compressionAlgorithm,
                             xCompressionError);
    }

    if (box != nullptr)
    {
        /* There is so little box data per frame that it is best to write multiple per chunk. */
        numFramesPerChunk = 20;
        wantedName        = "/particles/" + selectionName + "/box/edges";
        file->writeDataFrame(step,
                             time,
                             wantedName,
                             DIM,
                             DIM,
                             static_cast<const real*>(box[0]),
                             "nm",
                             numFramesPerChunk,
                             h5mdio::CompressionAlgorithm::LosslessNoShuffle);
    }

    /* There is no temporal compression of velocities and forces. */
    numFramesPerChunk = 1;
    if (v != nullptr)
    {
        wantedName = "/particles/" + selectionName + "/velocity";
        file->writeDataFrame(step,
                             time,
                             wantedName,
                             numParticles,
                             DIM,
                             static_cast<const real*>(v[0]),
                             "nm ps-1",
                             numFramesPerChunk,
                             h5mdio::CompressionAlgorithm::LosslessWithShuffle);
    }
    if (f != nullptr)
    {
        wantedName = "/particles/" + selectionName + "/force";
        file->writeDataFrame(step,
                             time,
                             wantedName,
                             numParticles,
                             DIM,
                             static_cast<const real*>(f[0]),
                             "kJ mol-1 nm-1",
                             numFramesPerChunk,
                             h5mdio::CompressionAlgorithm::LosslessWithShuffle);
    }
#else
    throw gmx::FileIOError(
            "GROMACS was compiled without HDF5 support, cannot handle this file type");
#endif
}

bool readNextFrameOfStandardDataBlocks(h5mdio::GmxH5mdIo* file,
                                       int64_t*           step,
                                       real*              time,
                                       real*              lambda,
                                       rvec*              box,
                                       rvec*              x,
                                       rvec*              v,
                                       rvec*              f,
                                       real*              xCompressionError,
                                       bool*              readLambda,
                                       bool*              readBox,
                                       bool*              readX,
                                       bool*              readV,
                                       bool*              readF,
                                       const std::string  selectionName)
{
    if (file == nullptr || !file->isFileOpen())
    {
        throw gmx::FileIOError("No file open for reading.");
    }

    std::string particlesNameStem = "/particles/" + selectionName;
    *readLambda = *readBox = *readX = *readV = *readF = false;

    std::tuple<int64_t, real> temporaryStepTime = file->getNextStepAndTimeToRead();
    *step                                       = std::get<0>(temporaryStepTime);
    *time                                       = std::get<1>(temporaryStepTime);

    bool didReadFrame  = false;
    *xCompressionError = -1;

    if (lambda != nullptr)
    {
        if (file->readNextFrameOfDataBlock("/observables/lambda", lambda, *step))
        {
            *readLambda  = true;
            didReadFrame = true;
        }
    }
    if (box != nullptr)
    {
        std::string boxDataName = particlesNameStem + "/box/edges";
        if (file->readNextFrameOfDataBlock(boxDataName.c_str(), static_cast<real*>(box[0]), *step))
        {
            *readBox     = true;
            didReadFrame = true;
        }
    }
    if (x != nullptr)
    {
        std::string xDataName = particlesNameStem + "/position";
        if (file->readNextFrameOfDataBlock(xDataName.c_str(), static_cast<real*>(x[0]), *step))
        {
            *readX             = true;
            didReadFrame       = true;
            *xCompressionError = file->getLossyCompressionErrorOfDataBlock(xDataName.c_str());
        }
    }
    if (v != nullptr)
    {
        std::string boxDataName = particlesNameStem + "/velocity";
        if (file->readNextFrameOfDataBlock(boxDataName.c_str(), static_cast<real*>(v[0]), *step))
        {
            *readV       = true;
            didReadFrame = true;
        }
    }
    if (f != nullptr)
    {
        std::string boxDataName = particlesNameStem + "/force";
        if (file->readNextFrameOfDataBlock(boxDataName.c_str(), static_cast<real*>(f[0]), *step))
        {
            *readF       = true;
            didReadFrame = true;
        }
    }
    return didReadFrame;
}

} // namespace gmx
