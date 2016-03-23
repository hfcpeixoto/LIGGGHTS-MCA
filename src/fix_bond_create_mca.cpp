/* ----------------------------------------------------------------------
    This is the

    ██╗     ██╗ ██████╗  ██████╗  ██████╗ ██╗  ██╗████████╗███████╗
    ██║     ██║██╔════╝ ██╔════╝ ██╔════╝ ██║  ██║╚══██╔══╝██╔════╝
    ██║     ██║██║  ███╗██║  ███╗██║  ███╗███████║   ██║   ███████╗
    ██║     ██║██║   ██║██║   ██║██║   ██║██╔══██║   ██║   ╚════██║
    ███████╗██║╚██████╔╝╚██████╔╝╚██████╔╝██║  ██║   ██║   ███████║
    ╚══════╝╚═╝ ╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚══════╝®

    DEM simulation engine, released by
    DCS Computing Gmbh, Linz, Austria
    http://www.dcs-computing.com, office@dcs-computing.com

    LIGGGHTS® is part of CFDEM®project:
    http://www.liggghts.com | http://www.cfdem.com

    Core developer and main author:
    Christoph Kloss, christoph.kloss@dcs-computing.com

    LIGGGHTS® is open-source, distributed under the terms of the GNU Public
    License, version 2 or later. It is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. You should have
    received a copy of the GNU General Public License along with LIGGGHTS®.
    If not, see http://www.gnu.org/licenses . See also top-level README
    and LICENSE files.

    LIGGGHTS® and CFDEM® are registered trade marks of DCS Computing GmbH,
    the producer of the LIGGGHTS® software and the CFDEM®coupling software
    See http://www.cfdem.com/terms-trademark-policy for details.

-------------------------------------------------------------------------
    Contributing author and copyright for this file:

    Alexey Smolin (ISPMS SB RAS, Tomsk, Russia, http://www.ispms.ru)

    Copyright 2016-     ISPMS SB RAS, Tomsk, Russia
------------------------------------------------------------------------- */

#include "math.h"
#include "mpi.h"
#include "string.h"
#include "stdlib.h"
#include "fix_bond_create_mca.h"
#include "update.h"
#include "respa.h"
#include "atom.h"
#include "force.h"
#include "pair.h"
#include "comm.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "random_mars.h"
#include "memory.h"
#include "error.h"
#include "modify.h"
#include <iostream>
#include "atom_vec_mca.h"

using namespace LAMMPS_NS;
using namespace FixConst;

#define BIG 1.0e20

using namespace MCAAtomConst;

/* ---------------------------------------------------------------------- */

FixBondCreateMCA::FixBondCreateMCA(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  if (narg < 9) error->all(FLERR,"Illegal fix bond/create/mca command");

  MPI_Comm_rank(world,&me);

  nevery = atoi(arg[3]);
  if (nevery <= 0) error->all(FLERR,"Illegal fix bond/create/mca command");

  force_reneighbor = 1;
  next_reneighbor = -1;
  vector_flag = 1;
  size_vector = 2;
  global_freq = 1;
  extvector = 0;

  iatomtype = atoi(arg[4]);
  jatomtype = atoi(arg[5]);
  double cutoff = atof(arg[6]);
  btype = atoi(arg[7]);
  newperts = atoi(arg[8]);

  if (iatomtype < 1 || iatomtype > atom->ntypes ||
      jatomtype < 1 || jatomtype > atom->ntypes)
    error->all(FLERR,"Invalid atom type in fix bond/create/mca command");
  if (cutoff < 0.0) error->all(FLERR,"Illegal fix bond/create/mca command");
  if (btype < 1 || btype > atom->nbondtypes)
    error->all(FLERR,"Invalid bond type in fix bond/create/mca command");

  cutsq = cutoff*cutoff;

  // optional keywords

  imaxbond = 0;
  inewtype = iatomtype;
  jmaxbond = 0;
  jnewtype = jatomtype;
  fraction = 1.0;
  int seed = 12345;

  int iarg = 9;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"iparam") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal fix bond/create/mca command");
      imaxbond = atoi(arg[iarg+1]);
      inewtype = atoi(arg[iarg+2]);
      if (imaxbond < 0) error->all(FLERR,"Illegal fix bond/create/mca command");
      if (inewtype < 1 || inewtype > atom->ntypes)
        error->all(FLERR,"Invalid atom type in fix bond/create/mca command");
      iarg += 3;
    } else if (strcmp(arg[iarg],"jparam") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal fix bond/create/mca command");
      jmaxbond = atoi(arg[iarg+1]);
      jnewtype = atoi(arg[iarg+2]);
      if (jmaxbond < 0) error->all(FLERR,"Illegal fix bond/create/mca command");
      if (jnewtype < 1 || jnewtype > atom->ntypes)
        error->all(FLERR,"Invalid atom type in fix bond/create/mca command");
      iarg += 3;
    } else if (strcmp(arg[iarg],"prob") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal fix bond/create/mca command");
      fraction = atof(arg[iarg+1]);
      seed = atoi(arg[iarg+2]);
      if (fraction < 0.0 || fraction > 1.0)
        error->all(FLERR,"Illegal fix bond/create/mca command");
      if (seed <= 0) error->all(FLERR,"Illegal fix bond/create/mca command");
      iarg += 3;
    } else error->all(FLERR,"Illegal fix bond/create/mca command");
  }

  // error check

  if (atom->molecular == 0)
    error->all(FLERR,"Cannot use fix bond/create/mca with non-molecular systems");
  if (iatomtype == jatomtype &&
      ((imaxbond != jmaxbond) || (inewtype != jnewtype)))
    error->all(FLERR,"Inconsistent iparam/jparam values in fix bond/create/mca command");

  // initialize Marsaglia RNG with processor-unique seed

  random = new RanMars(lmp,seed + me);

  // perform initial allocation of atom-based arrays
  // register with Atom class
  // bondcount values will be initialized in setup()

  bondcount = NULL;
  grow_arrays(atom->nmax);
  atom->add_callback(0);
  countflag = 0;

  // set comm sizes needed by this fix

  comm_forward = newperts + 2; //NP modified C.K. 2;
  comm_reverse = newperts + 1; //NP modified C.K. 2;

  // allocate arrays local to this fix

  nmax = 0;
  npartner = NULL;
  partner = NULL;
  probability = NULL;
  //NP modified C.K. distsq = NULL;

  // zero out stats

  createcount = 0;
  createcounttotal = 0;
}

/* ---------------------------------------------------------------------- */

FixBondCreateMCA::~FixBondCreateMCA()
{
  // unregister callbacks to this fix from Atom class

  atom->delete_callback(id,0);

  delete random;

  // delete locally stored arrays

  memory->sfree(bondcount);
  memory->sfree(npartner);
  memory->destroy(partner);
  memory->sfree(probability);
  //NP modified C.K. memory->sfree(distsq);

  //NP do _not_  delete this fix here - should stay active
  //NP modify->delete_fix("exchange_bonds_mca");
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::post_create()
{
    //NP register a fix to excange mca bonds across processors
   // char* fixarg[4];

   /* fixarg[0]="exchange_bonds_mca";
    fixarg[1]="all";
    fixarg[2]="bond/exchange/mca";
    modify->add_fix(3,fixarg);*/
}


/* ---------------------------------------------------------------------- */

int FixBondCreateMCA::setmask()
{
  int mask = 0;
  mask |= POST_INTEGRATE;
  mask |= POST_INTEGRATE_RESPA;
  return mask;
}

/* ---------------------------------------------------------------------- */

int FixBondCreateMCA::modify_param(int narg, char **arg)
{
    if(narg != 2) error->all(FLERR,"Illegal fix_modify command");
    if(strcmp(arg[0],"every") == 0) nevery = atoi(arg[1]);
    else error->all(FLERR,"Illegal fix_modify command");
    return 2;
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::init()
{
  if(force->pair == NULL) error->all(FLERR,"Fix bond/create/mca force->pair is NULL");

  if(!(force->bond_match("mca")))
     error->all(FLERR,"Fix bond/create can only be used together with dedicated 'mca' bond styles");

  // check cutoff for iatomtype,jatomtype - cutneighsq is used here
  double cutsq_limit = sqrt(force->pair->cutsq[iatomtype][jatomtype]) + neighbor->skin;
  cutsq_limit *= cutsq_limit;
  if (force->pair == NULL || cutsq > cutsq_limit)
    error->all(FLERR,"Fix bond/create/mca cutoff is longer than pairwise cutoff");
/*
  // require special bonds = 0,1,1

  int flag = 0;
  if (force->special_lj[1] != 0.0 || force->special_lj[2] != 1.0 ||
      force->special_lj[3] != 1.0) flag = 1;
  if (force->special_coul[1] != 0.0 || force->special_coul[2] != 1.0 ||
      force->special_coul[3] != 1.0) flag = 1;
  if (flag) error->all(FLERR,"Fix bond/create requires special_bonds = 0,1,1");

  // warn if angles, dihedrals, impropers are being used

  if (force->angle || force->dihedral || force->improper) {
    if (me == 0)
      error->warning(FLERR,"Created bonds will not create angles, "
                     "dihedrals, or impropers");
  }
*/
  // need a half neighbor list, built when ever re-neighboring occurs

  int irequest = neighbor->request((void *) this);
  neighbor->requests[irequest]->pair = 0;
  neighbor->requests[irequest]->fix = 1;

  if (strcmp(update->integrate_style,"respa") == 0)
    nlevels_respa = ((Respa *) update->integrate)->nlevels;
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::init_list(int id, NeighList *ptr)
{
  list = ptr;
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::setup(int vflag)
{
  int i,j,m;

  // compute initial bondcount if this is first run
  // can't do this earlier, like in constructor or init, b/c need ghost info

  if (countflag) return;
  countflag = 1;

  // count bonds stored with each bond I own
  // if newton bond is not set, just increment count on atom I
  // if newton bond is set, also increment count on atom J even if ghost
  // bondcount is long enough to tally ghost atom counts

  int *num_bond = atom->num_bond;
  int **bond_type = atom->bond_type;
  int **bond_atom = atom->bond_atom;
  int nlocal = atom->nlocal;
  int nghost = atom->nghost;
  int nall = nlocal + nghost;
  int newton_bond = force->newton_bond;

  for (i = 0; i < nall; i++) bondcount[i] = 0;

  for (i = 0; i < nlocal; i++)
    for (j = 0; j < num_bond[i]; j++) {
      if (bond_type[i][j] == btype) {
        bondcount[i]++;
        if (newton_bond) {
          m = atom->map(bond_atom[i][j]);
          if (m < 0)
            error->one(FLERR,"Could not count initial bonds in fix bond/create/mca");
          bondcount[m]++;
        }
      }
    }

  // if newton_bond is set, need to sum bondcount

  commflag = 0;
  if (newton_bond) comm->reverse_comm_fix(this);
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::post_integrate()
{
  int i,j,k,ii,jj,inum,jnum,itype,jtype,possible;
  double xtmp,ytmp,ztmp,delx,dely,delz,rsq,min,max;
  int *ilist,*jlist,*numneigh,**firstneigh;
  int flag;

//fprintf(logfile, "FixBondCreateMCA::post_integrate \n");///AS DEBUG
  if (nevery == 0 || update->ntimestep % nevery ) {
    return;
  }

  // need updated ghost atom positions

  comm->forward_comm();

  // forward comm of bondcount, so ghosts have it

  commflag = 0;
  comm->forward_comm_fix(this);

  // resize bond partner list and initialize it
  // probability array overlays distsq array
  // needs to be atom->nmax in length

  if (atom->nmax > nmax) {
    nmax = atom->nmax;
    memory->grow(partner,nmax,newperts,"fix_bond_create:partner");
    npartner = (int*) memory->srealloc(npartner,nmax*sizeof(int),"fix_bond_create:npartner");
    probability = (double *)memory->srealloc(probability,nmax*sizeof(double),"fix_bond_create:probability");
  }

  int nlocal = atom->nlocal;
  int nall = atom->nlocal + atom->nghost;

  for (i = 0; i < nall; i++) {
    for(int j = 0; j< newperts; j++) partner[i][j] = 0;
    npartner[i] = 0;
    probability[i] = 1.;
  }

  // loop over neighbors of my atoms
  // each atom sets one closest eligible partner atom ID to bond with

  double **x = atom->x;
  int *tag = atom->tag;
  int *mask = atom->mask;
  int *type = atom->type;

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  flag = 0;

  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    if (!(mask[i] & groupbit)) continue;
    itype = type[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    jlist = firstneigh[i];
    jnum = numneigh[i];

    for (jj = 0; jj < jnum; jj++) {

      j = jlist[jj];
      j &= NEIGHMASK;
      if (!(mask[j] & groupbit)) continue;
      jtype = type[j];

      possible = 0;
      if (itype == iatomtype && jtype == jatomtype) {
        if ((imaxbond == 0 || bondcount[i] < imaxbond) &&
            (jmaxbond == 0 || bondcount[j] < jmaxbond))
          possible = 1;
      } else if (itype == jatomtype && jtype == iatomtype) {
        if ((jmaxbond == 0 || bondcount[i] < jmaxbond) &&
            (imaxbond == 0 || bondcount[j] < imaxbond))
          possible = 1;
      }
      if (!possible) continue;

      delx = xtmp - x[j][0];
      dely = ytmp - x[j][1];
      delz = ztmp - x[j][2];
      rsq = delx*delx + dely*dely + delz*delz;
      if (rsq >= cutsq) continue;

      if(already_bonded(i,j)) {
        fprintf(logfile,"existing bond btw atoms %d and %d \n",i,j);
        continue;
      }

      if(npartner[i]==newperts || npartner[j]==newperts)
      {
          flag = 1;
          continue;
      }

      partner[i][npartner[i]] = tag[j];
      partner[j][npartner[j]] = tag[i];
      npartner[i]++;
      npartner[j]++;

    }
  }

  if(flag) error->warning(FLERR,"Could not generate all possible bonds");

  // reverse comm of distsq and partner
  // not needed if newton_pair off since I,J pair was seen by both procs

  commflag = 1;
  if (force->newton_pair) comm->reverse_comm_fix(this);

  // each atom now knows its partners
  // for prob check, generate random value for each atom with a bond partner
  // forward comm of partner and random value, so ghosts have it

  if (fraction < 1.0) {
    for (i = 0; i < nlocal; i++)
      if (npartner[i]) probability[i] = random->uniform();
  }

  commflag = 1;
  comm->forward_comm_fix(this);

  // create bonds for atoms I own
  // if other atom is owned by another proc, it should create same bond
  // if both atoms list each other as bond partner
  // and probability constraint is satisfied

  int **bond_type = atom->bond_type;
  int **bond_atom = atom->bond_atom;
  int *num_bond = atom->num_bond;
//  int **nspecial = atom->nspecial;
//  int **special = atom->special;
  int newton_bond = force->newton_bond;
  int n_bondhist = atom->n_bondhist;
  double ***bond_hist = atom->bond_hist;
  double r,rinv;

  int ncreate = 0;
  for (i = 0; i < nlocal; i++) {
    if (npartner[i] == 0) continue;

    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    for(k = 0; k < npartner[i]; k++)
    {
        j = atom->map(partner[i][k]);

        int found = 0;
        for(int jp = 0; jp < npartner[j]; jp++)
            if(partner[j][jp] == tag[i]) found = 1;
        if (!found) error->all(FLERR,"Internal fix bond/create error");

        // apply probability constraint
        // MIN,MAX insures values are added in same order on different procs

        if (fraction < 1.0) {
          min = MIN(probability[i],probability[j]);
          max = MAX(probability[i],probability[j]);
          if (0.5*(min+max) >= fraction) continue;
        }

        // if newton_bond is set, only store with I or J
        // if not newton_bond, store bond with both I and J

        if (!newton_bond /*|| tag[i] < tag[j]*/) 
        {
          fprintf(logfile,"creating bond btw atoms i=%d and j=%d (i has now %d bonds) at step %ld\n",i,j,num_bond[i]+1,update->ntimestep);

          if (num_bond[i] == atom->bond_per_atom)  
            error->one(FLERR,"New bond exceeded bonds per atom in fix bond/create");
          bond_type[i][num_bond[i]] = btype;  
          bond_atom[i][num_bond[i]] = tag[j];
          /*  print these lines
          std::cout << "if (!newton_bond || tag["<<i<<"] (="<<tag[i]<<") < tag["<<j<<"] (="<<tag[j]<<")) "<<std::endl; // NP P.F. correct this okt-29
          std::cout << "if (num_bond["<<i<<"] (="<<num_bond[i]<<")== atom->bond_per_atom(="<<atom->bond_per_atom<<"))  "<<std::endl; 
          std::cout << "bond_type["<<i<<"]["<<num_bond[i]<<"] = btype (="<<btype<<");"<<std::endl; 
          std::cout << "bond_atom["<<i<<"]["<<num_bond[i]<<"] = tag["<<j<<"] (="<<tag[j]<<");"<<std::endl; 
          */

          //reset history
          double *tmp = bond_hist[i][num_bond[i]];
          for (int ih = 0; ih < n_bondhist; ih++) {
              tmp[ih] = 0.;
          }

          delx = xtmp - x[j][0];
          dely = ytmp - x[j][1];
          delz = ztmp - x[j][2];
          r = sqrt(delx*delx + dely*dely + delz*delz);
          rinv = -1. / r; // "-" means that unit vector is from i1 to i2
          tmp[R] = tmp[R_PREV] = r;
          tmp[NX_PREV] = tmp[NX] = delx * rinv;
          tmp[NY_PREV] = tmp[NY] = dely * rinv;
          tmp[NZ_PREV] = tmp[NZ] = delz * rinv;

          num_bond[i]++;
        }
        // increment bondcount, convert atom to new type if limit reached

        bondcount[i]++;
        if (type[i] == iatomtype) {
          if (bondcount[i] == imaxbond) type[i] = inewtype; // bondtype defined by user in input script = iatomtype
        } else {
          if (bondcount[i] == jmaxbond) type[i] = jnewtype;
        }

        // count the created bond once

        if (tag[i] < tag[j]) ncreate++;
    }
  }

  // tally stats

  MPI_Allreduce(&ncreate,&createcount,1,MPI_INT,MPI_SUM,world);
  createcounttotal += createcount;
  atom->nbonds += createcount;

  if(createcount && comm->me == 0) fprintf(logfile,"Created %d unique bonds at timestep "BIGINT_FORMAT"\n",createcount,update->ntimestep);

  // trigger reneighboring if any bonds were formed

  if (createcount) next_reneighbor = update->ntimestep;

}

inline bool FixBondCreateMCA::already_bonded(int i,int j)
{
    for(int k = 0; k < atom->num_bond[i]; k++)
      if(atom->bond_atom[i][k] == atom->tag[j]) return true;
    return false;
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::post_integrate_respa(int ilevel, int iloop)
{
  if (ilevel == nlevels_respa-1) post_integrate();
}

/* ---------------------------------------------------------------------- */

int FixBondCreateMCA::pack_comm(int n, int *list, double *buf,
                             int pbc_flag, int *pbc)
{
  int i,j,m;

  m = 0;

  if (commflag == 0) {
    for (i = 0; i < n; i++) {
      j = list[i];
      buf[m++] = static_cast<int>(bondcount[j]);
    }
    return 1;

  } else {
    for (i = 0; i < n; i++) {
      j = list[i];
      buf[m++] = static_cast<int>(npartner[j]);
      for(int k=0; k<newperts; k++) //NP communicate all slots, also the empty ones
        buf[m++] = static_cast<int>(partner[j][k]);
      buf[m++] = probability[j];
    }
    return newperts + 2;
  }
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::unpack_comm(int n, int first, double *buf)
{
  int i,m,last;

  m = 0;
  last = first + n;

  if (commflag == 0) {
    for (i = first; i < last; i++)
      bondcount[i] = static_cast<int> (buf[m++]);

  } else {
    for (i = first; i < last; i++) {
      npartner[i] = static_cast<int> (buf[m++]);
      for(int k=0; k<newperts; k++)
        partner[i][k] = static_cast<int>(buf[m++]);
      probability[i] = buf[m++];
    }
  }
}

/* ---------------------------------------------------------------------- */

int FixBondCreateMCA::pack_reverse_comm(int n, int first, double *buf)
{
  int i,m,last;

  m = 0;
  last = first + n;

  if (commflag == 0) {
    for (i = first; i < last; i++)
      buf[m++] = bondcount[i];
    return 1;

  } else {
    for (i = first; i < last; i++) {
      buf[m++] = static_cast<int>(npartner[i]);
      for(int k=0; k<newperts; k++)
        buf[m++] = static_cast<int>(partner[i][k]);
    }
    return newperts + 2;
  }
}

/* ---------------------------------------------------------------------- */

void FixBondCreateMCA::unpack_reverse_comm(int n, int *list, double *buf)
{
  int i,j,m;

  m = 0;
  int flag = 0;
  int nnew;

  if (commflag == 0) {
    for (i = 0; i < n; i++) {
      j = list[i];
      bondcount[j] += static_cast<int> (buf[m++]);
    }
  } else {
    //NP add new bonds coming from other proc
    for (i = 0; i < n; i++) {
      j = list[i];
      nnew = static_cast<int> (buf[m++]);
      if(nnew+npartner[j] > newperts)
      {
          flag = 1;
          nnew = newperts - npartner[j];
      }
      for(int k = npartner[j]; k < npartner[j]+newperts; k++)
      {
          if(k >= npartner[j]+nnew) m++; //NP do not do anything if
          else partner[j][k] = static_cast<int> (buf[m++]);
      }
      npartner[i] += nnew;
    }
  }
  if(flag) error->warning(FLERR,"Could not generate all possible bonds");
}

/* ----------------------------------------------------------------------
   allocate local atom-based arrays
------------------------------------------------------------------------- */

void FixBondCreateMCA::grow_arrays(int nmax)
{
  bondcount = (int *)
    memory->srealloc(bondcount,nmax*sizeof(int),"bond/create:bondcount");
}

/* ----------------------------------------------------------------------
   copy values within local atom-based arrays
------------------------------------------------------------------------- */

void FixBondCreateMCA::copy_arrays(int i, int j)
{
  bondcount[j] = bondcount[i];
}

/* ----------------------------------------------------------------------
   pack values in local atom-based arrays for exchange with another proc
------------------------------------------------------------------------- */

int FixBondCreateMCA::pack_exchange(int i, double *buf)
{
  buf[0] = bondcount[i];
  return 1;
}

/* ----------------------------------------------------------------------
   unpack values in local atom-based arrays from exchange with another proc
------------------------------------------------------------------------- */

int FixBondCreateMCA::unpack_exchange(int nlocal, double *buf)
{
  bondcount[nlocal] = static_cast<int> (buf[0]);
  return 1;
}

/* ---------------------------------------------------------------------- */

double FixBondCreateMCA::compute_vector(int n)
{
  if (n == 1) return (double) createcount;
  return (double) createcounttotal;
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays
------------------------------------------------------------------------- */

double FixBondCreateMCA::memory_usage()
{
  int nmax = atom->nmax;
  double bytes = nmax*2 * sizeof(int);
  bytes += newperts*nmax * sizeof(int);
  bytes += nmax * sizeof(double);
  return bytes;
}
