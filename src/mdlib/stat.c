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

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "typedefs.h"
#include "sysstuff.h"
#include "gmx_fatal.h"
#include "network.h"
#include "txtdump.h"
#include "names.h"
#include "physics.h"
#include "vec.h"
#include "maths.h"
#include "mvdata.h"
#include "main.h"
#include "force.h"
#include "vcm.h"
#include "smalloc.h"
#include "futil.h"
#include "network.h"
#include "rbin.h"
#include "tgroup.h"
#include "xtcio.h"
#include "gmxfio.h"
#include "trnio.h"
#include "statutil.h"
#include "domdec.h"
#include "partdec.h"
#include "constr.h"
#include "checkpoint.h"
#include "mdrun.h"
#include "xvgr.h"

typedef struct gmx_global_stat
{
    t_bin *rb;
    int   *itc0;
    int   *itc1;
} t_gmx_global_stat;

gmx_global_stat_t global_stat_init(t_inputrec *ir)
{
    gmx_global_stat_t gs;

    snew(gs,1);
    
    gs->rb = mk_bin();
    snew(gs->itc0,ir->opts.ngtc);
    snew(gs->itc1,ir->opts.ngtc);

    return gs;
}

void global_stat_destroy(gmx_global_stat_t gs)
{
    destroy_bin(gs->rb);
    sfree(gs->itc0);
    sfree(gs->itc1);
    sfree(gs);
}

static int filter_enerdterm(real *afrom, gmx_bool bToBuffer, real *ato,
                            gmx_bool bTemp, gmx_bool bPres, gmx_bool bEner) {
    int i,to,from;

    from = 0;
    to   = 0;
    for (i=0;i<F_NRE;i++)
    {
        if (bToBuffer)
        {
            from = i;
        }
        else
        {
            to = i;
        }
        switch (i) {
        case F_EKIN:
        case F_TEMP:
        case F_DKDL:
            if (bTemp)
            {
                ato[to++] = afrom[from++];
            }
            break;
        case F_PRES:    
        case F_PDISPCORR:
        case F_VTEMP:
            if (bPres)
            {
                ato[to++] = afrom[from++];
            }
            break;
        default:
            if (bEner)
            {
                ato[to++] = afrom[from++];
            }
            break;
        }
    }

    return to;
}

void global_stat(FILE *fplog,gmx_global_stat_t gs,
                 t_commrec *cr,gmx_enerdata_t *enerd,
                 tensor fvir,tensor svir,rvec mu_tot,
                 t_inputrec *inputrec,
                 gmx_ekindata_t *ekind,gmx_constr_t constr,
                 t_vcm *vcm,
                 int nsig,real *sig,
                 gmx_mtop_t *top_global, t_state *state_local, 
                 gmx_bool bSumEkinhOld, int flags)
/* instead of current system, gmx_booleans for summing virial, kinetic energy, and other terms */
{
  t_bin  *rb;
  int    *itc0,*itc1;
  int    ie=0,ifv=0,isv=0,irmsd=0,imu=0;
  int    idedl=0,idvdll=0,idvdlnl=0,iepl=0,icm=0,imass=0,ica=0,inb=0;
  int    isig=-1;
  int    icj=-1,ici=-1,icx=-1;
  int    inn[egNR];
  real   copyenerd[F_NRE];
  int    nener,j;
  real   *rmsd_data=NULL;
  double nb;
  gmx_bool   bVV,bTemp,bEner,bPres,bConstrVir,bEkinAveVel,bFirstIterate,bReadEkin;

  bVV           = EI_VV(inputrec->eI);
  bTemp         = flags & CGLO_TEMPERATURE;
  bEner         = flags & CGLO_ENERGY;
  bPres         = (flags & CGLO_PRESSURE); 
  bConstrVir    = (flags & CGLO_CONSTRAINT);
  bFirstIterate = (flags & CGLO_FIRSTITERATE);
  bEkinAveVel   = (inputrec->eI==eiVV || (inputrec->eI==eiVVAK && bPres));
  bReadEkin     = (flags & CGLO_READEKIN);

  rb   = gs->rb;
  itc0 = gs->itc0;
  itc1 = gs->itc1;
  

  reset_bin(rb);
  /* This routine copies all the data to be summed to one big buffer
   * using the t_bin struct. 
   */

  /* First, we neeed to identify which enerd->term should be
     communicated.  Temperature and pressure terms should only be
     communicated and summed when they need to be, to avoid repeating
     the sums and overcounting. */

  nener = filter_enerdterm(enerd->term,TRUE,copyenerd,bTemp,bPres,bEner);
  
  /* First, the data that needs to be communicated with velocity verlet every time
     This is just the constraint virial.*/
  if (bConstrVir) {
      isv = add_binr(rb,DIM*DIM,svir[0]);
      where();
  }
  
/* We need the force virial and the kinetic energy for the first time through with velocity verlet */
  if (bTemp || !bVV)
  {
      if (ekind) 
      {
          for(j=0; (j<inputrec->opts.ngtc); j++) 
          {
              if (bSumEkinhOld) 
              {
                  itc0[j]=add_binr(rb,DIM*DIM,ekind->tcstat[j].ekinh_old[0]);
              }
              if (bEkinAveVel && !bReadEkin) 
              {
                  itc1[j]=add_binr(rb,DIM*DIM,ekind->tcstat[j].ekinf[0]);
              } 
              else if (!bReadEkin)
              {
                  itc1[j]=add_binr(rb,DIM*DIM,ekind->tcstat[j].ekinh[0]);
              }
          }
          /* these probably need to be put into one of these categories */
          where();
          idedl = add_binr(rb,1,&(ekind->dekindl));
          where();
          ica   = add_binr(rb,1,&(ekind->cosacc.mvcos));
          where();
      }  
  }      
  where();
  
  if ((bPres || !bVV) && bFirstIterate)
  {
      ifv = add_binr(rb,DIM*DIM,fvir[0]);
  }


  if (bEner) 
  { 
      where();
      if (bFirstIterate) 
      {
          ie  = add_binr(rb,nener,copyenerd);
      }
      where();
      if (constr) 
      {
          rmsd_data = constr_rmsd_data(constr);
          if (rmsd_data) 
          {
              irmsd = add_binr(rb,inputrec->eI==eiSD2 ? 3 : 2,rmsd_data);
          }
      } 
      if (!NEED_MUTOT(*inputrec)) 
      {
          imu = add_binr(rb,DIM,mu_tot);
          where();
      }
      
      if (bFirstIterate) 
      {
          for(j=0; (j<egNR); j++)
          {
              inn[j]=add_binr(rb,enerd->grpp.nener,enerd->grpp.ener[j]);
          }
          where();
          if (inputrec->efep != efepNO) 
          {
              idvdll  = add_bind(rb,1,&enerd->dvdl_lin);
              idvdlnl = add_bind(rb,1,&enerd->dvdl_nonlin);
              if (enerd->n_lambda > 0) 
              {
                  iepl = add_bind(rb,enerd->n_lambda,enerd->enerpart_lambda);
              }
          }
      }

      if (vcm) 
      {
          icm   = add_binr(rb,DIM*vcm->nr,vcm->group_p[0]);
          where();
          imass = add_binr(rb,vcm->nr,vcm->group_mass);
          where();
          if (vcm->mode == ecmANGULAR) 
          {
              icj   = add_binr(rb,DIM*vcm->nr,vcm->group_j[0]);
              where();
              icx   = add_binr(rb,DIM*vcm->nr,vcm->group_x[0]);
              where();
              ici   = add_binr(rb,DIM*DIM*vcm->nr,vcm->group_i[0][0]);
              where();
          }
      }
  }
  if (DOMAINDECOMP(cr)) 
  {
      nb = cr->dd->nbonded_local;
      inb = add_bind(rb,1,&nb);
      }
  where();
  if (nsig > 0) 
  {
      isig = add_binr(rb,nsig,sig);
  }

  /* Global sum it all */
  if (debug)
  {
      fprintf(debug,"Summing %d energies\n",rb->maxreal);
  }
  sum_bin(rb,cr);
  where();

  /* Extract all the data locally */

  if (bConstrVir) 
  {
      extract_binr(rb,isv ,DIM*DIM,svir[0]);
  }

  /* We need the force virial and the kinetic energy for the first time through with velocity verlet */
  if (bTemp || !bVV)
  {
      if (ekind) 
      {
          for(j=0; (j<inputrec->opts.ngtc); j++) 
          {
              if (bSumEkinhOld)
              {
                  extract_binr(rb,itc0[j],DIM*DIM,ekind->tcstat[j].ekinh_old[0]);
              }
              if (bEkinAveVel && !bReadEkin) {
                  extract_binr(rb,itc1[j],DIM*DIM,ekind->tcstat[j].ekinf[0]);
              }
              else if (!bReadEkin)
              {
                  extract_binr(rb,itc1[j],DIM*DIM,ekind->tcstat[j].ekinh[0]);              
              }
          }
          extract_binr(rb,idedl,1,&(ekind->dekindl));
          extract_binr(rb,ica,1,&(ekind->cosacc.mvcos));
          where();
      }
  }
  if ((bPres || !bVV) && bFirstIterate)
  {
      extract_binr(rb,ifv ,DIM*DIM,fvir[0]);
  }

  if (bEner) 
  {
      if (bFirstIterate) 
      {
          extract_binr(rb,ie,nener,copyenerd);
          if (rmsd_data) 
          {
              extract_binr(rb,irmsd,inputrec->eI==eiSD2 ? 3 : 2,rmsd_data);
          }
          if (!NEED_MUTOT(*inputrec))
          {
              extract_binr(rb,imu,DIM,mu_tot);
          }

          for(j=0; (j<egNR); j++)
          {
              extract_binr(rb,inn[j],enerd->grpp.nener,enerd->grpp.ener[j]);
          }
          if (inputrec->efep != efepNO) 
          {
              extract_bind(rb,idvdll ,1,&enerd->dvdl_lin);
              extract_bind(rb,idvdlnl,1,&enerd->dvdl_nonlin);
              if (enerd->n_lambda > 0) 
              {
                  extract_bind(rb,iepl,enerd->n_lambda,enerd->enerpart_lambda);
              }
          }
          /* should this be here, or with ekin?*/
          if (vcm) 
          {
              extract_binr(rb,icm,DIM*vcm->nr,vcm->group_p[0]);
              where();
              extract_binr(rb,imass,vcm->nr,vcm->group_mass);
              where();
              if (vcm->mode == ecmANGULAR) 
              {
                  extract_binr(rb,icj,DIM*vcm->nr,vcm->group_j[0]);
                  where();
                  extract_binr(rb,icx,DIM*vcm->nr,vcm->group_x[0]);
                  where();
                  extract_binr(rb,ici,DIM*DIM*vcm->nr,vcm->group_i[0][0]);
                  where();
              }
          }
          if (DOMAINDECOMP(cr)) 
          {
              extract_bind(rb,inb,1,&nb);
              if ((int)(nb + 0.5) != cr->dd->nbonded_global) 
              {
                  dd_print_missing_interactions(fplog,cr,(int)(nb + 0.5),top_global,state_local);
              }
          }
          where();

          filter_enerdterm(copyenerd,FALSE,enerd->term,bTemp,bPres,bEner);    
/* Small hack for temp only - not entirely clear if still needed?*/
          /* enerd->term[F_TEMP] /= (cr->nnodes - cr->npmenodes); */
      }
  }

  if (nsig > 0) 
  {
      extract_binr(rb,isig,nsig,sig);
  }
  where();
}

int do_per_step(gmx_large_int_t step,gmx_large_int_t nstep)
{
  if (nstep != 0) 
    return ((step % nstep)==0); 
  else 
    return 0;
}

static void moveit(t_commrec *cr,
		   int left,int right,const char *s,rvec xx[])
{
  if (!xx) 
    return;

  move_rvecs(cr,FALSE,FALSE,left,right,
	     xx,NULL,(cr->nnodes-cr->npmenodes)-1,NULL);
}

gmx_mdoutf_t *init_mdoutf(int nfile,const t_filenm fnm[],int mdrun_flags,
                          const t_commrec *cr,const t_inputrec *ir,
                          const output_env_t oenv)
{
    gmx_mdoutf_t *of;
    char filemode[3];
    gmx_bool bAppendFiles;

    snew(of,1);

    of->fp_trn   = NULL;
    of->fp_ene   = NULL;
    of->fp_xtc   = NULL;
    of->fp_dhdl  = NULL;
    of->fp_field = NULL;
    
    of->eIntegrator     = ir->eI;
    of->simulation_part = ir->simulation_part;


	bAppendFiles = (mdrun_flags & MD_APPENDFILES);

	of->bKeepAndNumCPT = (mdrun_flags & MD_KEEPANDNUMCPT);

	sprintf(filemode, bAppendFiles ? "a+" : "w+");
        
	if (MASTER(cr))
	{
        if ((EI_DYNAMICS(ir->eI) || EI_ENERGY_MINIMIZATION(ir->eI))
#ifndef GMX_FAHCORE
            &&
            !(EI_DYNAMICS(ir->eI) &&
              ir->nstxout == 0 &&
              ir->nstvout == 0 &&
              ir->nstfout == 0)
#endif
	    )
        {
            of->fp_trn = open_trn(ftp2fn(efTRN,nfile,fnm), filemode);
        }

        if (EI_DYNAMICS(ir->eI) || EI_ENERGY_MINIMIZATION(ir->eI))
        {
            of->fp_ene = open_enx(ftp2fn(efEDR,nfile,fnm), filemode);
        }
        of->fn_cpt = opt2fn("-cpo",nfile,fnm);
        
        if (ir->efep != efepNO && ir->nstdhdl > 0 &&
            (ir->separate_dhdl_file == sepdhdlfileYES ) && 
            EI_DYNAMICS(ir->eI))
        {
            if (bAppendFiles)
            {
                of->fp_dhdl = gmx_fio_fopen(opt2fn("-dhdl",nfile,fnm),filemode);
            }
            else
            {
                of->fp_dhdl = open_dhdl(opt2fn("-dhdl",nfile,fnm),ir,oenv);
            }
        }
        
        if (opt2bSet("-field",nfile,fnm) &&
            (ir->ex[XX].n || ir->ex[YY].n || ir->ex[ZZ].n))
        {
            if (bAppendFiles)
            {
                of->fp_dhdl = gmx_fio_fopen(opt2fn("-field",nfile,fnm),
                                            filemode);
            }
            else
            {				  
                of->fp_field = xvgropen(opt2fn("-field",nfile,fnm),
                                        "Applied electric field","Time (ps)",
                                        "E (V/nm)",oenv);
            }
        }
    }
    if (!EI_DYNAMICS(ir->eI) &&
        ir->nstxtcout > 0)
    {
        of->fp_xtc = open_xtc(ftp2fn(efXTC,nfile,fnm), filemode, cr->dd);
        of->xtc_prec = ir->xtcprec;
    }
    return of;
}

void done_mdoutf(gmx_mdoutf_t *of)
{
    if (of->fp_ene != NULL)
    {
        close_enx(of->fp_ene);
    }
    if (of->fp_xtc)	//if (dd->rank < NUMBEROFSTEPS)
    {
        close_xtc(of->fp_xtc);
    }
    if (of->fp_trn)
    {
        close_trn(of->fp_trn);
    }
    if (of->fp_dhdl != NULL)
    {
        gmx_fio_fclose(of->fp_dhdl);
    }
    if (of->fp_field != NULL)
    {
        gmx_fio_fclose(of->fp_field);
    }

    sfree(of);
}

int copy_state_local(t_state *new_sl,t_state *old_sl)
{

	new_sl->cg_gl_nalloc = 0;//NOTE: This is done so that we can test to see if the memcpy successfully copied the old state_local to the new state_local

	int *cg_gl_new = new_sl->cg_gl;
	rvec *x_new = new_sl->x;

	memcpy (new_sl,old_sl,sizeof(t_state));
	new_sl->cg_gl = cg_gl_new;
	new_sl->x = x_new;
	memcpy (new_sl->cg_gl, old_sl->cg_gl, sizeof(int) * old_sl->cg_gl_nalloc);
	memcpy (new_sl->x, old_sl->x, sizeof(rvec) * old_sl->natoms);


	//If it fails to memcpy return 0
	if (new_sl->cg_gl_nalloc != old_sl->cg_gl_nalloc)// Testing to see if the copy worked by checking to see if these two ints are equal
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

void write_traj(FILE *fplog,t_commrec *cr,
                gmx_mdoutf_t *of,
                int mdof_flags,
                gmx_mtop_t *top_global,
                gmx_large_int_t step,double t,
                t_state *state_local,t_state *state_global,
                rvec *f_local,rvec *f_global,
                int *n_xtc,rvec **x_xtc,
                t_inputrec *ir, gmx_bool bLastStep)//TODO: RJ - MAY break code elsewhere
{
    int     i,j;
    gmx_groups_t *groups;
    rvec    *xxtc;
    rvec *local_v;
    rvec *global_v;
    
    // RJ - These are my code additions
//    const int NUMBEROFSTEPS = 2; // RJ - This is how many times a node will save its own data sending the data to the master node.
    							 // It also determines how many nodes will collect the frames.


    static gmx_domdec_t **dd_buf = NULL;//TODO: Don't use static
    static t_state **state_local_buf = NULL;
    //static rvec **x_receive_buf = NULL;// this is only going to be used when all 50 frames are being written.
	static gmx_large_int_t step_buf;
	static double t_buf;
	static int step_at_checkpoint = 0; /*first step or first step after checkpoint*/
	int bufferStep;
	gmx_bool writeXTCNow;

	if (step_at_checkpoint == 0) {
		step_at_checkpoint = ir->init_step;
	}

	bufferStep = (step/ir->nstxtcout - (int)ceil((double)step_at_checkpoint/ir->nstxtcout))%cr->dd->n_xtc_steps;// bufferStep = step/(how often to write) - (round up) step_at_checkpoint/(how often to write)  MOD (how often we actually do write)
	writeXTCNow = ((mdof_flags & MDOF_XTC) && bufferStep == cr->dd->n_xtc_steps-1) || bLastStep || (mdof_flags & MDOF_CPT) || (mdof_flags & MDOF_X);//True if the buffer is full OR its the last step OR its a checkpoint

	if ((mdof_flags & MDOF_CPT) || (mdof_flags & MDOF_X))
	{
		step_at_checkpoint = step + 1;
	}

    if (dd_buf==NULL) {//Initializes the dd_buf
    	initialize_dd_buf(&dd_buf, cr->dd, state_local);
    }
    if (state_local_buf==NULL)//Initializes the state_local_buf
    {
    	snew (state_local_buf, cr->dd->n_xtc_steps);
    	for (i=0;i<cr->dd->n_xtc_steps;i++)
    	{
    		snew(state_local_buf[i],1);
    		snew(state_local_buf[i]->cg_gl,state_local->cg_gl_nalloc);
    		snew(state_local_buf[i]->x,state_local->nalloc);
    	}
    }
    if (state_global->x == NULL && cr->dd->rank<cr->dd->n_xtc_steps ) {//Initializes the state_global->x for the total number of atoms
    	snew(state_global->x, state_global->natoms);
    }
    // RJ - End of additions


#define MX(xvf) moveit(cr,GMX_LEFT,GMX_RIGHT,#xvf,xvf)


    /* MRS -- defining these variables is to manage the difference
     * between half step and full step velocities, but there must be a better way . . . */

    local_v  = state_local->v;
    global_v = state_global->v;

    if (DOMAINDECOMP(cr))
    {
        if (mdof_flags & MDOF_CPT)

        {
            dd_collect_state(cr->dd,state_local,state_global);
        }
        else
        {
            if (mdof_flags &
            		(MDOF_X))
            {
                dd_collect_vec(cr->dd,state_local,state_local->x,
                               state_global->x);
            }
            if (mdof_flags & MDOF_V)
            {
                dd_collect_vec(cr->dd,state_local,local_v,
                               global_v);
            }
        }
        if (mdof_flags & MDOF_F)
        {
            dd_collect_vec(cr->dd,state_local,f_local,f_global);
        }
        //TODO: RJ We can optimize by not collecting all but xtc selection.
        if (mdof_flags & MDOF_XTC)
        {
        	//TODO: DEALLOCATE later, now allocation in this method. all outside where dd and state_local is allocated

        	//This block of code copies the current dd and state_local to buffers to prepare for writing later.
        	if ((writeXTCNow && cr->dd->rank == 0) || (!writeXTCNow && bufferStep == cr->dd->n_xtc_steps-1 - cr->dd->rank))
        	{
        		step_buf=step;
        		t_buf=t;
        	}
    		srenew(state_local_buf[bufferStep]->cg_gl,state_local->cg_gl_nalloc);
    		srenew(state_local_buf[bufferStep]->x,state_local->nalloc);
        	copy_dd(dd_buf[bufferStep],cr->dd,state_local);
        	copy_state_local(state_local_buf[bufferStep],state_local);


        	if (writeXTCNow)
        	{
        		for (i = 0; i <= bufferStep; i++)//This loop changes which node is the master node temporarily so that it can then write
        										 // the frames to different nodes.
        		{
        			if (i==bufferStep)
        			{
        				dd_buf[i]->masterrank = 0;
        			}
        			else
        			{
        				dd_buf[i]->masterrank = cr->dd->n_xtc_steps-1 - i;// For the purpose of making checkpoints work correctly: we write the frames in reverse order
        			}
        			if (!(i==bufferStep && ((mdof_flags & MDOF_CPT) || (mdof_flags & MDOF_X))))
        			{
        				dd_collect_vec(dd_buf[i],state_local_buf[i],state_local_buf[i]->x,state_global->x);
        			}
        		}
        	}
        }
    }
    else  //TODO make sure particle decomposition works . e.g. always use serial files in that case
    {
        if (mdof_flags & MDOF_CPT)
        {
            /* All pointers in state_local are equal to state_global,
             * but we need to copy the non-pointer entries.
             */
            state_global->lambda = state_local->lambda;
            state_global->veta = state_local->veta;
            state_global->vol0 = state_local->vol0;
            copy_mat(state_local->box,state_global->box);
            copy_mat(state_local->boxv,state_global->boxv);
            copy_mat(state_local->svir_prev,state_global->svir_prev);
            copy_mat(state_local->fvir_prev,state_global->fvir_prev);
            copy_mat(state_local->pres_prev,state_global->pres_prev);
        }
        if (cr->nnodes > 1)
        {
            /* Particle decomposition, collect the data on the master node */
            if (mdof_flags & MDOF_CPT)
            {
                if (state_local->flags & (1<<estX))   MX(state_global->x);
                if (state_local->flags & (1<<estV))   MX(state_global->v);
                if (state_local->flags & (1<<estSDX)) MX(state_global->sd_X);
                if (state_global->nrngi > 1) {
                    if (state_local->flags & (1<<estLD_RNG)) {
#ifdef GMX_MPI // RJ - MPI means that the processors are going to be sharing data.
                        MPI_Gather(state_local->ld_rng ,
                                   state_local->nrng*sizeof(state_local->ld_rng[0]),MPI_BYTE,
                                   state_global->ld_rng,

                                   state_local->nrng*sizeof(state_local->ld_rng[0]),MPI_BYTE,
                                   MASTERRANK(cr),cr->mpi_comm_mygroup);
#endif
                    }
                    if (state_local->flags & (1<<estLD_RNGI))
                    {
#ifdef GMX_MPI
                        MPI_Gather(state_local->ld_rngi,
                                   sizeof(state_local->ld_rngi[0]),MPI_BYTE,
                                   state_global->ld_rngi,
                                   sizeof(state_local->ld_rngi[0]),MPI_BYTE,
                                   MASTERRANK(cr),cr->mpi_comm_mygroup);
#endif
                    }
                }
            }
            else
            {
                if (mdof_flags & (MDOF_X | MDOF_XTC)) MX(state_global->x);
                if (mdof_flags & MDOF_V)              MX(global_v);
            }
            if (mdof_flags & MDOF_F) MX(f_global);
         }
     }

     if (MASTER(cr))
     {
         if (mdof_flags & MDOF_CPT)
         {
             /*write_checkpoint(of->fn_cpt,of->bKeepAndNumCPT,
                              fplog,cr,of->eIntegrator,
                              of->simulation_part,step,t,state_global);*/ //TODO!
         }

         if (mdof_flags & (MDOF_X | MDOF_V | MDOF_F))
         {
            fwrite_trn(of->fp_trn,step,t,state_local->lambda,
                       state_local->box,top_global->natoms,
                       (mdof_flags & MDOF_X) ? state_global->x : NULL,
                       (mdof_flags & MDOF_V) ? global_v : NULL,
                       (mdof_flags & MDOF_F) ? f_global : NULL);
            if (gmx_fio_flush(of->fp_trn) != 0)
            {
                gmx_file("Cannot write trajectory; maybe you are out of quota?");
            }
            gmx_fio_check_file_position(of->fp_trn);
        }      
     }

     if (writeXTCNow && cr->dd->rank < cr->dd->n_xtc_steps) {  //this is an IO node (we have to call write_traj on all IO nodes!)


		gmx_bool bWrite = (cr->dd->n_xtc_steps - bufferStep <= cr->dd->rank ) || (cr->dd->rank == 0);  //this node is actually writing
		if (bWrite) { // If this node is one of the writing nodes
			groups = &top_global->groups;
			if (*n_xtc == -1)
			{
				*n_xtc = 0;
				for(i=0; (i<top_global->natoms); i++)
				{
					if (ggrpnr(groups,egcXTC,i) == 0)
					{
						(*n_xtc)++;
					}
				}
				if (*n_xtc != top_global->natoms)
				{
					snew(*x_xtc,*n_xtc);
				}
			}
			if (*n_xtc == top_global->natoms)
			{
				xxtc = state_global->x;
			}
			else
			{
				xxtc = *x_xtc;
				j = 0;
				for(i=0; (i<top_global->natoms); i++)
				{
					if (ggrpnr(groups,egcXTC,i) == 0)
					{
						copy_rvec (state_global->x[i], xxtc[j++]);

					}
				}
			}
		}
		if (write_xtc(of->fp_xtc,*n_xtc,step_buf,t_buf,
				  state_local->box,xxtc,of->xtc_prec,!bWrite) == 0)//If it is NOT ACTUALLY being written
		{
			gmx_fatal(FARGS,"XTC error - maybe you are out of quota?");
		}
		//gmx_fio_check_file_position(of->fp_xtc); //TODO: temporary should be reactivated! than check that appending works, MPI_File_get_position_shared, MPI_File_seek_shared
	}
}

