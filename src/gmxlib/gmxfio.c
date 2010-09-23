/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
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
 * 
 * And Hey:
 * GROningen Mixture of Alchemy and Childrens' Stories
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#ifdef HAVE_IO_H
#include <io.h>
#endif

#include "gmx_fatal.h"
#include "macros.h"
#include "smalloc.h"
#include "futil.h"
#include "filenm.h"
#include "string2.h"
#include "gmxfio.h"
#include "md5.h"

#ifdef GMX_THREADS
#include "thread_mpi.h"
#endif

#include "gmxfio_int.h"

/* This is the new improved and thread safe version of gmxfio. */



/* the list of open files is a linked list, with a dummy element at its head; 
       it is initialized when the first file is opened. */
static t_fileio *open_files = NULL;


#ifdef GMX_THREADS
/* this mutex locks the open_files structure so that no two threads can 
   modify it.        

   For now, we use this as a coarse grained lock on all file 
   insertion/deletion operations because it makes avoiding deadlocks 
   easier, and adds almost no overhead: the only overhead is during
   opening and closing of files, or during global operations like
   iterating along all open files. All these cases should be rare
   during the simulation. */
static tMPI_Thread_mutex_t open_file_mutex=TMPI_THREAD_MUTEX_INITIALIZER;
#endif


/* These simple lists define the I/O type for these files */
static const int ftpXDR[] =
    { efTPR, efTRR, efEDR, efXTC, efMTX, efCPT };
static const int ftpASC[] =
    { efTPA, efGRO, efPDB };
static const int ftpBIN[] =
    { efTPB, efTRJ };
#ifdef HAVE_XML
static const int ftpXML[] =
    {   efXML};
#endif

const char *itemstr[eitemNR] =
    { "[header]", "[inputrec]", "[box]", "[topology]", "[coordinates]",
                "[velocities]", "[forces]" };

const char *eioNames[eioNR] =
    { "REAL", "INT", "GMX_STE_T", "UCHAR", "NUCHAR", "USHORT", "RVEC", "NRVEC",
                "IVEC", "STRING" };



/* Comment strings for TPA only */
const char *comment_str[eitemNR] = {
"; The header holds information on the number of atoms etc. and on whether\n"
"; certain items are present in the file or not.\n"
"; \n"
";                             WARNING\n"
";                   DO NOT EDIT THIS FILE BY HAND\n"
"; The GROMACS preprocessor performs a lot of checks on your input that\n"
"; you ignore when editing this. Your simulation may crash because of this\n",
"; The inputrec holds the parameters for MD such as the number of steps,\n"
"; the timestep and the cut-offs.\n",
"; The simulation box in nm.\n",
"; The topology section describes the topology of the molecules\n"
"; i.e. bonds, angles and dihedrals etc. and also holds the force field\n"
"; parameters.\n", 
"; The atomic coordinates in nm\n",
"; The atomic velocities in nm/ps\n",
"; The forces on the atoms in nm/ps^2\n" };




/******************************************************************
 *
 * Internal functions: 
 *
 ******************************************************************/

static int gmx_fio_int_flush(t_fileio* fio)
{
    int rc = 0;

    if (fio->fp)
    {
        rc = fflush(fio->fp);
    }
    else if (fio->xdr)
    {
        rc = fflush((FILE *) fio->xdr->x_private);
    }

    return rc;
}

/* returns TRUE if the file type ftp is in the set set */
static gmx_bool in_ftpset(int ftp, int nset, const int set[])
{
    int i;
    gmx_bool bResult;

    bResult = FALSE;
    for (i = 0; (i < nset); i++)
        if (ftp == set[i])
            bResult = TRUE;

    return bResult;
}



extern void gmx_fio_set_comment(t_fileio *fio, const char *comment)
{
    fio->comment=comment;
}

extern void gmx_fio_unset_comment(t_fileio *fio)
{
    fio->comment=NULL;
}


const char *gmx_fio_dbgstr(t_fileio *fio, const char *desc, char *buf)
{
    if (!fio->bDebug)
    {
        /* set to empty string */
        buf[0]=0;
    }
    else
    {
#if (defined( _WIN32 ) || defined( _WIN64 ) )
        /* windows doesn't do standard C */
#define snprintf sprintf_s
#endif
        snprintf(buf, GMX_FIO_BUFLEN, "  ; %s %s", 
                 fio->comment ? fio->comment : "", desc);
    }
    return buf;
}


/* check the number of items given against the type */
void gmx_fio_check_nitem(t_fileio *fio, int eio, int nitem, const char *file, 
                         int line)
{
    if ((nitem != 1) && !((eio == eioNRVEC) || (eio == eioNUCHAR)))
        gmx_fatal(FARGS, 
                  "nitem (%d) may differ from 1 only for %s or %s, not   for %s"
                  "(%s, %d)",nitem,eioNames[eioNUCHAR],eioNames[eioNRVEC],
                  eioNames[eio],file,line);
}


/* output a data type error. */
void gmx_fio_fe(t_fileio *fio, int eio, const char *desc, 
                const char *srcfile, int line)
{

    gmx_fatal(FARGS, "Trying to %s %s type %d (%s), src %s, line %d",
              fio->bRead ? "read" : "write",desc,eio,
              ((eio >= 0) && (eio < eioNR)) ? eioNames[eio] : "unknown",
              srcfile,line);
}


/* set the reader/writer functions based on the file type */
static void gmx_fio_set_iotype(t_fileio *fio)
{
    if (in_ftpset(fio->iFTP, asize(ftpXDR), ftpXDR))
    {
#ifdef USE_XDR    
        fio->iotp=&xdr_iotype;
#else
        gmx_fatal(FARGS,"Sorry, no XDR");
#endif
    }
    else if (in_ftpset(fio->iFTP, asize(ftpASC), ftpASC))
    {
        fio->iotp=&asc_iotype;
    }
    else if (in_ftpset(fio->iFTP, asize(ftpBIN), ftpBIN))
    {
        fio->iotp=&bin_iotype;
    }
#ifdef HAVE_XMl
    else if (in_ftpset(fio->iFTP,asize(ftpXML),ftpXML))
    {
        fio->iotp=&dummy_iotype;
    }
#endif
    else
        fio->iotp=&dummy_iotype;
}


/* lock the mutex associated with this fio. This needs to be done for every
   type of access to the fio's elements. */
void gmx_fio_lock(t_fileio *fio)
{
#ifdef GMX_THREADS
    tMPI_Lock_lock(&(fio->mtx));
#endif
}
/* unlock the mutex associated with this fio.  */
void gmx_fio_unlock(t_fileio *fio)
{
#ifdef GMX_THREADS
    tMPI_Lock_unlock(&(fio->mtx));
#endif
}

/* make a dummy head element, assuming we locked everything. */
static void gmx_fio_make_dummy(void)
{
    if (!open_files)
    {
        snew(open_files,1);
        open_files->fp=NULL;
        open_files->fn=NULL;
        open_files->next=open_files;
        open_files->prev=open_files;
#ifdef GMX_THREADS
        tMPI_Lock_init(&(open_files->mtx));
#endif
    }
}







/***********************************************************************
 *
 * FILE LIST OPERATIONS
 *
***********************************************************************/


/* insert a new t_fileio into the list */
static void gmx_fio_insert(t_fileio *fio)
{
    t_fileio *prev;
#ifdef GMX_THREADS
    /* first lock the big open_files mutex. */
    tMPI_Thread_mutex_lock(&open_file_mutex);
#endif
    /* now check whether the dummy element has been allocated, 
       and allocate it if it hasn't */
    gmx_fio_make_dummy();

    /* and lock the fio we got and the list's head **/
    gmx_fio_lock(fio);
    gmx_fio_lock(open_files);
    prev=open_files->prev;
    /* lock the element after the current one */
    if (prev != open_files)
    {
        gmx_fio_lock(prev);
    }

    /* now do the actual insertion: */
    fio->next=open_files;
    open_files->prev=fio;
    prev->next=fio;
    fio->prev=prev;

    /* now unlock all our locks */
    if (prev != open_files)
    {
        gmx_fio_unlock(prev);
    }
    gmx_fio_unlock(open_files);
    gmx_fio_unlock(fio);

#ifdef GMX_THREADS
    /* now unlock the big open_files mutex.  */
    tMPI_Thread_mutex_unlock(&open_file_mutex);
#endif
}

/* remove a t_fileio into the list. We assume the fio is locked, and we leave 
   it locked. 
   NOTE: We also assume that the open_file_mutex has been locked */
static void gmx_fio_remove(t_fileio *fio)
{    
    t_fileio *prev;

    /* lock prev, because we're changing it */ 
    gmx_fio_lock(fio->prev);

    /* now set the prev's pointer */
    fio->prev->next=fio->next;
    gmx_fio_unlock(fio->prev);

    /* with the next ptr, we can simply lock while the original was locked */
    gmx_fio_lock(fio->next);
    fio->next->prev=fio->prev;
    gmx_fio_unlock(fio->next);

    /* and make sure we point nowhere in particular */
    fio->next=fio->prev=fio;
}


/* get the first open file, or NULL if there is none. 
   Returns a locked fio. */
static t_fileio *gmx_fio_get_first(void)
{
    t_fileio *ret;
    /* first lock the big open_files mutex and the dummy's mutex */

#ifdef GMX_THREADS
    /* first lock the big open_files mutex. */
    tMPI_Thread_mutex_lock(&open_file_mutex);
#endif
    gmx_fio_make_dummy();

    gmx_fio_lock(open_files);
    ret=open_files->next;


    /* check whether there were any to begin with */
    if (ret==open_files)
    {
        /* after this, the open_file pointer should never change */
        ret=NULL;
    }
    else
    {
        gmx_fio_lock(open_files->next);
    }
    gmx_fio_unlock(open_files);


    return ret;
}

/* get the next open file, or NULL if there is none. 
   Unlocks the previous fio and locks the next one. */
static t_fileio *gmx_fio_get_next(t_fileio *fio)
{
    t_fileio *ret;

    ret=fio->next;
    /* check if that was the last one */
    if (fio->next==open_files)
    {
        ret=NULL;
#ifdef GMX_THREADS
        tMPI_Thread_mutex_unlock(&open_file_mutex);
#endif
    }
    else
    {
        gmx_fio_lock(ret);
    }
    gmx_fio_unlock(fio);

    return ret;
}

/* Stop looping through the open_files.  Unlocks the global lock. */
static void gmx_fio_stop_getting_next(t_fileio *fio)
{
    gmx_fio_unlock(fio);
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&open_file_mutex);
#endif
}


static int gmx_write_it(char *handle, char *buf, int size)
{
	t_fileio *fio = (t_fileio*) handle;
	if (fio->mem_buf_nalloc<fio->mem_buf_cur_pos+size) {
		fio->mem_buf_nalloc = (fio->mem_buf_cur_pos+size)*1.10;
		srenew(fio->mem_buf,fio->mem_buf_nalloc);
	}
	memcpy(fio->mem_buf+fio->mem_buf_cur_pos, buf+4, size-4);  //Remove 4-byte record-marker, see http://docs.sun.com/app/docs/doc/816-1435/6m7rrfn9i?l=en&a=view#rpcproto-14265
	fio->mem_buf_cur_pos += size-4;
	return size;
}

/*****************************************************************
 *
 *                     EXPORTED SECTION
 *
 *****************************************************************/


t_fileio *gmx_fio_open(const char *fn, const char *mode)
{
   return mpi_fio_open(fn,mode,NULL);
}

void gmx_fio_start_record(t_fileio *fio)
{
	fio->mem_buf_cur_pos = 0;
}

t_fileio *mpi_fio_open(const char *fn, const char *mode, gmx_domdec_t *dd)
{
    t_fileio *fio = NULL;
    int i;
    char newmode[5];
    gmx_bool bRead, bReadWrite;
    int xdrid;

    if (fn2ftp(fn) == efTPA)
    {
        strcpy(newmode, mode);
    }
    else
    {
        /* sanitize the mode string */
        if (strncmp(mode, "r+", 2) == 0)
        {
            strcpy(newmode, "r+");
        }
        else if (mode[0] == 'r')
        {
            strcpy(newmode, "r");
        }
        else if (strncmp(mode, "w+", 2) == 0)
        {
            strcpy(newmode, "w+");
        }
        else if (mode[0] == 'w')
        {
            strcpy(newmode, "w");
        }
        else if (strncmp(mode, "a+", 2) == 0)
        {
            strcpy(newmode, "a+");
        }
        else if (mode[0] == 'a')
        {
            strcpy(newmode, "a");
        }
        else
        {
            gmx_fatal(FARGS, "DEATH HORROR in gmx_fio_open, mode is '%s'",mode);
        }
    }

    /* Check if it should be opened as a binary file */
    if (strncmp(ftp2ftype(fn2ftp(fn)),"ASCII",5))
    {
        /* Not ascii, add b to file mode */
        if ((strchr(newmode,'b')==NULL) && (strchr(newmode,'B')==NULL))
        {
            strcat(newmode,"b");
        }
    }

    snew(fio, 1);
#ifdef GMX_THREADS
    tMPI_Lock_init(&(fio->mtx));
#endif
    bRead = (newmode[0]=='r' && newmode[1]!='+');
    bReadWrite = (newmode[1]=='+');
    fio->fp = NULL;
    fio->xdr = NULL;
    if (fn)
    {
        fio->iFTP = fn2ftp(fn);
        fio->fn = strdup(fn);
        fio->bStdio = FALSE;

        /* If this file type is in the list of XDR files, open it like that */
        if (in_ftpset(fio->iFTP,asize(ftpXDR),ftpXDR))
        {
            /* First check whether we have to make a backup,
             * only for writing, not for read or append.
             */
            if (newmode[0]=='w')
            {
#ifndef GMX_FAHCORE
                /* only make backups for normal gromacs */
                make_backup(fn);
#endif
            }
            else
            {
                /* Check whether file exists */
                if (!gmx_fexist(fn))
                {
                    gmx_open(fn);
                }
            }

            /* determine the XDR direction */
            if (newmode[0] == 'w' || newmode[0]=='a')
            {
                fio->xdrmode=XDR_ENCODE;
            }
            else
            {
                fio->xdrmode=XDR_DECODE;
            }

            snew(fio->xdr,1);

#ifdef GMX_LIB_MPI
            if (dd!=NULL)
            {
            	MPI_Comm new_comm;
            	int amode;
            	if (strcmp(mode,"w+")==0) {
            		amode = MPI_MODE_RDWR | MPI_MODE_CREATE;
            	} else if (strcmp(mode,"a+")==0) {
            		amode = MPI_MODE_RDWR | MPI_MODE_APPEND;
            	} else if (strcmp(mode,"r")==0) {
            		amode = MPI_MODE_RDONLY;
            	} else {
            		gmx_fatal(FARGS,"Unknown mode!");
            	}

            	xdrrec_create(fio->xdr,0,0,(char*)fio,NULL,&gmx_write_it);

            	MPI_Comm_split(dd->mpi_comm_all, dd->rank < dd->n_xtc_steps, dd->nnodes - dd->rank, &new_comm );// new_comm must be a vector of size color // total nodes - rank
            	if (dd->rank < dd->n_xtc_steps)
            	{
            		MPI_File_open(new_comm,(char*)fn,amode,MPI_INFO_NULL, &(fio->mpi_fh));
            	}
            }
            else
            {
#endif
            	/* Open the file without MPI */
            	fio->fp = ffopen(fn,newmode);

                xdrstdio_create(fio->xdr, fio->fp, fio->xdrmode);

#ifdef GMX_MPI
            }
#endif


        }
        else
        {
            /* If it is not, open it as a regular file */
            fio->fp = ffopen(fn,newmode);
        }
    }
    else
    {
        /* Use stdin/stdout for I/O */
        fio->iFTP   = efTPA;
        fio->fp     = bRead ? stdin : stdout;
        fio->fn     = strdup("STDIO");
        fio->bStdio = TRUE;
    }
    fio->bRead  = bRead;
    fio->bReadWrite = bReadWrite;
    fio->bDouble= (sizeof(real) == sizeof(double));
    fio->bDebug = FALSE;
    fio->bOpen  = TRUE;
    fio->bLargerThan_off_t = FALSE;

    /* set the reader/writer functions */
    gmx_fio_set_iotype(fio);

    /* and now insert this file into the list of open files. */
    gmx_fio_insert(fio);
    return fio;
}



static int gmx_fio_close_locked(t_fileio *fio)
{
    int rc = 0;

    if (!fio->bOpen)
    {
        gmx_fatal(FARGS,"File %s closed twice!\n", fio->fn);
    }

    if (in_ftpset(fio->iFTP, asize(ftpXDR), ftpXDR))
    {
        xdr_destroy(fio->xdr);
        sfree(fio->xdr);
    }

#ifdef GMX_LIB_MPI
    if (fio->mpi_fh != NULL)
    {
    	MPI_File_close(&(fio->mpi_fh));
    }
    else
#endif
    {
		/* Don't close stdin and stdout! */
		if (!fio->bStdio && fio->fp!=NULL)
			rc = ffclose(fio->fp); /* fclose returns 0 if happy */
    }

    fio->bOpen = FALSE;

    return rc;
}

int gmx_fio_close(t_fileio *fio)
{
    int rc = 0;

#ifdef GMX_THREADS
    /* first lock the big open_files mutex. */
    /* We don't want two processes operating on the list at the same time */
    tMPI_Thread_mutex_lock(&open_file_mutex);
#endif

    gmx_fio_lock(fio);
    /* first remove it from the list */
    gmx_fio_remove(fio);
    rc=gmx_fio_close_locked(fio);
    gmx_fio_unlock(fio);

    sfree(fio);

#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&open_file_mutex);
#endif

    return rc;
}

/* close only fp but keep FIO entry. */
int gmx_fio_fp_close(t_fileio *fio)
{
    int rc=0;
    gmx_fio_lock(fio);
    if (!in_ftpset(fio->iFTP,asize(ftpXDR),ftpXDR) && !fio->bStdio)
    {
        rc = ffclose(fio->fp); /* fclose returns 0 if happy */
        fio->fp = NULL; 
    }
    gmx_fio_unlock(fio);

    return rc;
}

FILE * gmx_fio_fopen(const char *fn, const char *mode)
{
    FILE *fp, *ret;
    t_fileio *fio;

    fio = gmx_fio_open(fn, mode);
    gmx_fio_lock(fio);
    ret = fio->fp;
    gmx_fio_unlock(fio);

    return ret;
}

int gmx_fio_fclose(FILE *fp)
{
    t_fileio *cur;
    t_fileio *found=NULL;
    int rc=-1;

    cur=gmx_fio_get_first();
    while(cur)
    {
        if (cur->fp == fp)
        {
            rc=gmx_fio_close_locked(cur);
            gmx_fio_remove(cur);
            gmx_fio_stop_getting_next(cur);
            break;
        }
        cur=gmx_fio_get_next(cur);
    }

    return rc;
}

/* internal variant of get_file_md5 that operates on a locked file */
static int gmx_fio_int_get_file_md5(t_fileio *fio, gmx_off_t offset, 
                                    unsigned char digest[])
{
    /*1MB: large size important to catch almost identical files */
#define CPT_CHK_LEN  1048576 
    md5_state_t state;
    unsigned char buf[CPT_CHK_LEN];
    gmx_off_t read_len;
    gmx_off_t seek_offset;
    int ret = -1;

    seek_offset = offset - CPT_CHK_LEN;
    if (seek_offset < 0)
    {
        seek_offset = 0;
    }
    read_len = offset - seek_offset;


    if (fio->fp && fio->bReadWrite)
    {
        ret=gmx_fseek(fio->fp, seek_offset, SEEK_SET);
        if (ret)
        {
            gmx_fseek(fio->fp, 0, SEEK_END);
        }
    }
    if (ret) /*either no fp, not readwrite, or fseek not successful */
    {
        return -1;
    }

    /* the read puts the file position back to offset */
    if ((gmx_off_t)fread(buf, 1, read_len, fio->fp) != read_len)
    {
        /* not fatal: md5sum check to prevent overwriting files
         * works (less safe) without
         * */
        if (ferror(fio->fp))
        {
            fprintf(stderr, "\nTrying to get md5sum: %s: %s\n", fio->fn,
                    strerror(errno));
        }
        else if (feof(fio->fp))
        {
            /*
             * For long runs that checkpoint frequently but write e.g. logs
             * infrequently we don't want to issue lots of warnings before we
             * have written anything to the log.
             */
            if(0)
            {
                fprintf(stderr, "\nTrying to get md5sum: EOF: %s\n", fio->fn);
            }
        }
        else
        {
            fprintf(
                stderr,
                "\nTrying to get md5sum: Unknown reason for short read: %s\n",
                fio->fn);
        }

        gmx_fseek(fio->fp, 0, SEEK_END);

        ret = -1;
    }
    gmx_fseek(fio->fp, 0, SEEK_END); /*is already at end, but under windows 
                                       it gives problems otherwise*/

    if (debug)
    {
        fprintf(debug, "chksum %s readlen %ld\n", fio->fn, (long int)read_len);
    }

    if (!ret)
    {
        md5_init(&state);
        md5_append(&state, buf, read_len);
        md5_finish(&state, digest);
        return read_len;
    }
    else
    {
        return ret;
    }
}


/*
 * fio: file to compute md5 for
 * offset: starting pointer of region to use for md5
 * digest: return array of md5 sum 
 */
int gmx_fio_get_file_md5(t_fileio *fio, gmx_off_t offset, 
                         unsigned char digest[])
{
    int ret;

    gmx_fio_lock(fio);
    ret=gmx_fio_int_get_file_md5(fio, offset, digest);
    gmx_fio_unlock(fio);

    return ret;
}

/* The fio_mutex should ALWAYS be locked when this function is called */
static int gmx_fio_int_get_file_position(t_fileio *fio, gmx_off_t *offset)
{
    char buf[STRLEN];

    /* Flush the file, so we are sure it is written */
    if (gmx_fio_int_flush(fio))
    {
        char buf[STRLEN];
        sprintf(
            buf,
            "Cannot write file '%s'; maybe you are out of disk space or quota?",
            fio->fn);
        gmx_file(buf);
    }

    /* We cannot count on XDR being able to write 64-bit integers, 
     so separate into high/low 32-bit values.
     In case the filesystem has 128-bit offsets we only care 
     about the first 64 bits - we'll have to fix
     this when exabyte-size output files are common...
     */
    *offset=gmx_ftell(fio->fp);  //TODO: replace all calls to gmx_ftell/fseek with gmx_fio_tell/seek. in their test for MPI

    return 0;
}

int gmx_fio_check_file_position(t_fileio *fio)
{
    /* If gmx_off_t is 4 bytes we can not store file offset > 2 GB.
     * If we do not have ftello, we will play it safe.
     */
#if (SIZEOF_GMX_OFF_T == 4 || !defined HAVE_FSEEKO)
    gmx_off_t offset;

    gmx_fio_lock(fio);
    gmx_fio_int_get_file_position(fio,&offset);
    /* We have a 4 byte offset,
     * make sure that we will detect out of range for all possible cases.
     */
    if (offset < 0 || offset > 2147483647)
    {
        fio->bLargerThan_off_t = TRUE;
    }
    gmx_fio_unlock(fio);
#endif

    return 0;
}

int gmx_fio_get_output_file_positions(gmx_file_position_t **p_outputfiles,
                                      int *p_nfiles)
{
    int i, nfiles, rc, nalloc;
    int pos_hi, pos_lo;
    long pos;
    gmx_file_position_t * outputfiles;
    char buf[STRLEN];
    t_fileio *cur;

    nfiles = 0;

    /* pre-allocate 100 files */
    nalloc = 100;
    snew(outputfiles,nalloc);

    cur=gmx_fio_get_first();
    while(cur)
    {
        /* Skip the checkpoint files themselves, since they could be open when 
           we call this routine... */
        /* also skip debug files (shoud be the only iFTP==efNR) */
        if (cur->bOpen && 
            !cur->bRead && 
            !cur->bStdio && 
            cur->iFTP != efCPT && 
            cur->iFTP != efNR)
        {
            int ret;
            /* This is an output file currently open for writing, add it */
            if (nfiles == nalloc)
            {
                nalloc += 100;
                srenew(outputfiles,nalloc);
            }

            strncpy(outputfiles[nfiles].filename, cur->fn, STRLEN - 1);

            /* Get the file position */
            if (cur->bLargerThan_off_t)
            {
                /* -1 signals out of range */
                outputfiles[nfiles].offset = -1;
                outputfiles[nfiles].chksum_size = -1;
            }
            else
            {
                gmx_fio_int_get_file_position(cur, &outputfiles[nfiles].offset);
#ifndef GMX_FAHCORE
                outputfiles[nfiles].chksum_size
                    = gmx_fio_int_get_file_md5(cur, 
                                               outputfiles[nfiles].offset,
                                               outputfiles[nfiles].chksum);
#endif
            }

            nfiles++;
        }

        cur=gmx_fio_get_next(cur);
    }
    *p_nfiles = nfiles;
    *p_outputfiles = outputfiles;

    return 0;
}


void gmx_fio_checktype(t_fileio *fio)
{
    if (in_ftpset(fio->iFTP, asize(ftpXDR), ftpXDR))
    {
        return;
    }
    else if (in_ftpset(fio->iFTP, asize(ftpASC), ftpASC))
    {
        return;
    }
    else if (in_ftpset(fio->iFTP, asize(ftpBIN), ftpBIN))
    {
        return;
    }
#ifdef HAVE_XMl
    else if (in_ftpset(fio->iFTP,asize(ftpXML),ftpXML))
    {
        return;
    }
#endif
    else
        gmx_fatal(FARGS, "Can not read/write topologies to file type %s",
                  ftp2ext(fio->iFTP));

}


void gmx_fio_setprecision(t_fileio *fio, gmx_bool bDouble)
{
    gmx_fio_lock(fio);
    fio->bDouble = bDouble;
    gmx_fio_unlock(fio);
}

gmx_bool gmx_fio_getdebug(t_fileio *fio)
{
    gmx_bool ret;

    gmx_fio_lock(fio);
    ret = fio->bDebug;
    gmx_fio_unlock(fio);

    return ret;
}

void gmx_fio_setdebug(t_fileio *fio, gmx_bool bDebug)
{
    gmx_fio_lock(fio);
    fio->bDebug = bDebug;
    gmx_fio_unlock(fio);
}

char *gmx_fio_getname(t_fileio *fio)
{
    char *ret;
    gmx_fio_lock(fio);
    ret = fio->fn;
    gmx_fio_unlock(fio);

    return ret;
}

int gmx_fio_getftp(t_fileio* fio)
{
    int ret;

    gmx_fio_lock(fio);
    ret = fio->iFTP;
    gmx_fio_unlock(fio);

    return ret;
}

void gmx_fio_rewind(t_fileio* fio)
{
    gmx_fio_lock(fio);

    if (fio->xdr)
    {
        xdr_destroy(fio->xdr);
        frewind(fio->fp);
        xdrstdio_create(fio->xdr, fio->fp, fio->xdrmode);
    }
    else
    {
        frewind(fio->fp);
    }
    gmx_fio_unlock(fio);
}


int gmx_fio_flush(t_fileio* fio)
{
    int ret = 0;
    gmx_fio_lock(fio);
#ifdef GMX_LIB_MPI
    if (fio->mpi_fh != NULL) {
    	xdrrec_endofrecord(fio->xdr, 1);
        MPI_File_write_ordered(fio->mpi_fh,fio->mem_buf,fio->mem_buf_cur_pos,MPI_BYTE,MPI_STATUS_IGNORE);
    }
    else
#endif
    {
    	ret=gmx_fio_int_flush(fio);
    }
    gmx_fio_unlock(fio);
    return ret;
}



static int gmx_fio_int_fsync(t_fileio *fio)
{
    int rc = 0;
    int filen=-1;


    if (fio->fp)
    {
        rc=gmx_fsync(fio->fp);
    }
    else if (fio->xdr) /* this should normally not happen */
    {
        rc=gmx_fsync((FILE*) fio->xdr->x_private);
                                    /* ^ is this actually OK? */
    }

    return rc;
}


int gmx_fio_fsync(t_fileio *fio)
{
    int rc;

    gmx_fio_lock(fio);
    rc=gmx_fio_int_fsync(fio);
    gmx_fio_unlock(fio);

    return rc;
}



t_fileio *gmx_fio_all_output_fsync(void)
{
    t_fileio *ret=NULL;
    t_fileio *cur;

    cur=gmx_fio_get_first();
    while(cur)
    {
        /* skip debug files (shoud be the only iFTP==efNR) */
        if (cur->bOpen && 
            !cur->bRead && 
            !cur->bStdio && 
            cur->iFTP != efNR)
        {
            /* if any of them fails, return failure code */
            int rc=gmx_fio_int_fsync(cur);
            if (rc != 0 && !ret) 
            {
                ret=cur;
            }
        }
        cur=gmx_fio_get_next(cur);
    }

    /* in addition, we force these to be written out too, if they're being
       redirected. We don't check for errors because errors most likely mean
       that they're not redirected. */
    fflush(stdout);
    fflush(stderr);
#if (defined(HAVE_FSYNC))
    /* again, fahcore defines HAVE_FSYNC and fsync() */
    fsync(STDOUT_FILENO);
    fsync(STDERR_FILENO);
#endif

    return ret;
}


gmx_off_t gmx_fio_ftell(t_fileio* fio)
{
    gmx_off_t ret = 0;

    gmx_fio_lock(fio);
    if (fio->fp)
        ret = gmx_ftell(fio->fp);
    gmx_fio_unlock(fio);
    return ret;
}

int gmx_fio_seek(t_fileio* fio, gmx_off_t fpos)
{
    int rc;

    gmx_fio_lock(fio);
    if (fio->fp)
    {
        gmx_fseek(fio->fp, fpos, SEEK_SET);
    }
    else
    {
        gmx_file(fio->fn);
        rc = -1;
    }
    gmx_fio_unlock(fio);
    return rc;
}

FILE *gmx_fio_getfp(t_fileio *fio)
{
    FILE *ret = NULL;

    gmx_fio_lock(fio);
    if (fio->fp)
        ret = fio->fp;
    gmx_fio_unlock(fio);
    return ret;
}

XDR *gmx_fio_getxdr(t_fileio* fio)
{
    XDR *ret = NULL;

    gmx_fio_lock(fio);
    if (fio->xdr)
        ret = fio->xdr;
    gmx_fio_unlock(fio);

    return ret;
}

gmx_bool gmx_fio_getread(t_fileio* fio)
{
    gmx_bool ret;

    gmx_fio_lock(fio);
    ret = fio->bRead;
    gmx_fio_unlock(fio);

    return ret;
}

int xtc_seek_frame(t_fileio *fio, int frame, int natoms)
{
    int ret;

    gmx_fio_lock(fio);
    ret=xdr_xtc_seek_frame(frame, fio->fp, fio->xdr, natoms);
    gmx_fio_unlock(fio);

    return ret;
}

int xtc_seek_time(t_fileio *fio, real time, int natoms)
{
    int ret;

    gmx_fio_lock(fio);
    ret=xdr_xtc_seek_time(time, fio->fp, fio->xdr, natoms);
    gmx_fio_unlock(fio);

    return ret;
}


