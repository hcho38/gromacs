/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2009, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 *
 * For more info, check our website at http://www.gromacs.org
 */
/*! \file
 * \brief
 * Declares gmx::AnalysisData and related classes.
 *
 * \author Teemu Murtola <teemu.murtola@cbr.su.se>
 * \inpublicapi
 * \ingroup module_analysisdata
 */
#ifndef GMX_ANALYSISDATA_ANALYSISDATA_H
#define GMX_ANALYSISDATA_ANALYSISDATA_H

#include "abstractdata.h"

namespace gmx
{

class AnalysisDataHandle;
class AnalysisDataParallelOptions;

/*! \brief
 * Parallelizable data container for raw data.
 *
 * \if internal
 * Special note for MPI implementation: assuming that the initialization of
 * data objects is identical in all processes, associating the data objects
 * in different MPI processes should be possible without changes in the
 * interface.
 * Alternative, more robust implementation could get a unique ID as parameter
 * to the constructor or a separate function, but would require all tools to
 * provide it.
 * \endif
 *
 * \inpublicapi
 * \ingroup module_analysisdata
 */
class AnalysisData : public AbstractAnalysisData
{
    public:
        //! Creates an empty analysis data object.
        AnalysisData();
        virtual ~AnalysisData();

        /*! \brief
         * Sets the number of columns in the data and type of the data.
         *
         * \see isMultipoint()
         */
        void setColumns(int ncol, bool multipoint = false);

        /*! \brief
         * Create a handle for adding data.
         *
         * \param[in]  opt     Options for setting how this handle will be
         *     used.
         * \returns The created handle.
         */
        AnalysisDataHandle *startData(const AnalysisDataParallelOptions &opt);
        /*! \brief
         * Destroy a handle after all data has been added.
         *
         * \param[in]  handle  Handle to destroy.
         *
         * The pointer \p handle is invalid after the call.
         */
        void finishData(AnalysisDataHandle *handle);

    private:
        virtual AnalysisDataFrameRef tryGetDataFrameInternal(int index) const;
        virtual bool requestStorageInternal(int nframes);

        class Impl;

        Impl                *impl_;

        friend class AnalysisDataHandle;

        // Copy and assign disallowed by base class.
};


/*! \brief
 * Handle for inserting data into AnalysisData.
 *
 * Several handles can exist concurrently.
 *
 * \inpublicapi
 * \ingroup module_analysisdata
 */
class AnalysisDataHandle
{
    public:
        /*! \brief
         * Frees memory allocated for the internal implementation.
         *
         * Should not be called directly, but through finishData() or
         * AnalysisData::finishData().
         */
        ~AnalysisDataHandle();

        //! Start data for a new frame.
        void startFrame(int index, real x, real dx = 0.0);
        //! Set a value for a single column for the current frame.
        void setPoint(int col, real y, real dy = 0.0, bool present = true);
        //! Set values for consecutive columns for the current frame.
        void setPoints(int firstcol, int n, const real *y);
        //! Finish data for the current point set.
        void finishPointSet();
        //! Finish data for the current frame.
        void finishFrame();
        //! Calls AnalysisData::finishData() for this handle.
        void finishData();

    private:
        /*! \brief
         * Creates a new data handle associated with \p data.
         *
         * \param  data Data to associate the handle with.
         *
         * The constructor is private because data handles should only be
         * constructed through AnalysisData::startData().
         */
        explicit AnalysisDataHandle(AnalysisData *data);

        class Impl;

        Impl                   *impl_;

        friend class AnalysisData;

        // Disallow copy and assign.
        AnalysisDataHandle(const AnalysisDataHandle &);
        void operator =(const AnalysisDataHandle &);
};

} // namespace gmx

#endif
