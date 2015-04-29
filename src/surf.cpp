/* ----------------------------------------------------------------------
   SPARTA - Stochastic PArallel Rarefied-gas Time-accurate Analyzer
   http://sparta.sandia.gov
   Steve Plimpton, sjplimp@sandia.gov, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2014) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level SPARTA directory.
------------------------------------------------------------------------- */

#include "surf.h"
#include "math_extra.h"
#include "style_surf_collide.h"
#include "style_surf_react.h"
#include "surf_collide.h"
#include "surf_react.h"
#include "domain.h"
#include "comm.h"
#include "cut3d.h"
#include "geometry.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"

using namespace SPARTA_NS;
using namespace MathConst;

enum{TALLYAUTO,TALLYREDUCE,TALLYLOCAL};         // same as Update

#define DELTA 4
#define EPSSQ 1.0e-12
#define BIG 1.0e20
#define MAXGROUP 32

/* ---------------------------------------------------------------------- */

Surf::Surf(SPARTA *sparta) : Pointers(sparta)
{
  exist = 0;

  gnames = (char **) memory->smalloc(MAXGROUP*sizeof(char *),"surf:gnames");
  bitmask = (int *) memory->smalloc(MAXGROUP*sizeof(int),"surf:bitmask");
  for (int i = 0; i < MAXGROUP; i++) bitmask[i] = 1 << i;

  ngroup = 1;
  int n = strlen("all") + 1;
  gnames[0] = new char[n];
  strcpy(gnames[0],"all");

  npoint = nline = ntri = 0;
  pts = NULL;
  lines = NULL;
  tris = NULL;
  pushflag = 0;

  nlocal = 0;
  mysurfs = NULL;

  nsc = maxsc = 0;
  sc = NULL;

  nsr = maxsr = 0;
  sr = NULL;

  tally_comm = TALLYAUTO;
}

/* ---------------------------------------------------------------------- */

Surf::~Surf()
{
  for (int i = 0; i < ngroup; i++) delete [] gnames[i];
  memory->sfree(gnames);
  memory->sfree(bitmask);

  memory->sfree(pts);
  memory->sfree(lines);
  memory->sfree(tris);
  memory->sfree(mysurfs);

  for (int i = 0; i < nsc; i++) delete sc[i];
  memory->sfree(sc);
  for (int i = 0; i < nsr; i++) delete sr[i];
  memory->sfree(sr);
}

/* ---------------------------------------------------------------------- */

void Surf::modify_params(int narg, char **arg)
{
  if (narg < 2) error->all(FLERR,"Illegal surf_modify command");
  int igroup = find_group(arg[0]);
  if (igroup < 0) error->all(FLERR,"Surf_modify surface group is not defined");
  int groupbit = bitmask[igroup];

  int iarg = 1;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"collide") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal surf_modify command");

      int isc = find_collide(arg[iarg+1]);
      if (isc < 0) error->all(FLERR,"Could not find surf_modify sc-ID");

      // set surf collision model for each surf in surface group

      if (domain->dimension == 2) {
        for (int i = 0; i < nline; i++)
          if (lines[i].mask & groupbit) lines[i].isc = isc;
      }
      if (domain->dimension == 3) {
        for (int i = 0; i < ntri; i++)
          if (tris[i].mask & groupbit) tris[i].isc = isc;
      }

      iarg += 2;

    } else if (strcmp(arg[iarg],"react") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal surf_modify command");
      
      int isr;
      if (strcmp(arg[iarg+1],"none") == 0) isr = -1;
      else {
        isr = find_react(arg[iarg+1]);
        if (isr < 0) error->all(FLERR,"Could not find surf_modify sr-ID");
      }

      // set surf reaction model for each surf in surface group

      if (domain->dimension == 2) {
        for (int i = 0; i <= nline; i++)
          if (lines[i].mask & groupbit) lines[i].isr = isr;
      }
      if (domain->dimension == 3) {
        for (int i = 0; i <= ntri; i++)
          if (tris[i].mask & groupbit) tris[i].isr = isr;
      }

      iarg += 2;

    } else error->all(FLERR,"Illegal surf_modify command");
  }
}

/* ---------------------------------------------------------------------- */

void Surf::init()
{
  // check that every element is assigned to a surf collision model

  int flag = 0;
  if (domain->dimension == 2) {
    for (int i = 0; i < nline; i++)
      if (lines[i].isc < 0) flag++;
  } 
  if (domain->dimension == 3) {
    for (int i = 0; i < ntri; i++)
      if (tris[i].isc < 0) flag++;
  }
  if (flag) {
    char str[64];
    sprintf(str,"%d surface elements not assigned to a collision model",flag);
    error->all(FLERR,str);
  }

  // initialize surf collision and reaction models

  for (int i = 0; i < nsc; i++) sc[i]->init();
  for (int i = 0; i < nsr; i++) sr[i]->init();
}

/* ----------------------------------------------------------------------
   return # of lines or triangles
------------------------------------------------------------------------- */

int Surf::nelement()
{
  if (domain->dimension == 2) return nline;
  return ntri;
}

/* ----------------------------------------------------------------------
   setup owned surf elements
   create mysurfs list of owned surfs
   compute local index for owned cells
------------------------------------------------------------------------- */

void Surf::setup_surf()
{
  int me = comm->me;
  int nprocs = comm->nprocs;

  int n = nelement();

  // assign every Pth surf element to this proc

  nlocal = n/nprocs;
  if (me < n % nprocs) nlocal++;

  memory->destroy(mysurfs);
  memory->create(mysurfs,nlocal,"surf:mysurfs");

  nlocal = 0;
  for (int m = me; m < n; m += nprocs)
    mysurfs[nlocal++] = m;

  // set bounding box of all surfs based on pts
  // for 2d, set zlo,zhi to box bounds

  int i;
  for (i = 0; i < 3; i++) {
    bblo[i] = BIG;
    bbhi[i] = -BIG;
  }
  
  double *x;
  for (int ipt = 0; ipt < npoint; ipt++) {
    x = pts[ipt].x;
    for (i = 0; i < 3; i++) {
      bblo[i] = MIN(bblo[i],x[i]);
      bbhi[i] = MAX(bbhi[i],x[i]);
    }
  }

  if (domain->dimension == 2) {
    bblo[2] = domain->boxlo[2];
    bbhi[2] = domain->boxhi[2];
  }
}

/* ----------------------------------------------------------------------
   compute unit outward normal vectors of N lines starting at Nstart
   outward normal = +z axis x (p2-p1)
------------------------------------------------------------------------- */

void Surf::compute_line_normal(int nstart, int n)
{
  int p1,p2;
  double z[3],delta[3],norm[3];

  z[0] = 0.0; z[1] = 0.0; z[2] = 1.0;

  int m = nstart;
  for (int i = 0; i < n; i++) {
    p1 = lines[m].p1;
    p2 = lines[m].p2;
    MathExtra::sub3(pts[p2].x,pts[p1].x,delta);
    MathExtra::cross3(z,delta,norm);
    MathExtra::norm3(norm);
    lines[m].norm[0] = norm[0];
    lines[m].norm[1] = norm[1];
    lines[m].norm[2] = 0.0;
    m++;
  }
}

/* ----------------------------------------------------------------------
   compute unit outward normal vectors of N triangles starting at Nstart
   outward normal = (p2-p1) x (p3-p1)
------------------------------------------------------------------------- */

void Surf::compute_tri_normal(int nstart, int n)
{
  int p1,p2,p3;
  double delta12[3],delta13[3];

  int m = nstart;
  for (int i = 0; i < n; i++) {
    p1 = tris[m].p1;
    p2 = tris[m].p2;
    p3 = tris[m].p3;
    MathExtra::sub3(pts[p2].x,pts[p1].x,delta12);
    MathExtra::sub3(pts[p3].x,pts[p1].x,delta13);
    MathExtra::cross3(delta12,delta13,tris[m].norm);
    MathExtra::norm3(tris[m].norm);
    m++;
  }
}

/* ----------------------------------------------------------------------
   return coords of a corner point in a 2d quad
   icorner pts 1 to 4 are ordered by x, then by y
------------------------------------------------------------------------- */

void Surf::quad_corner_point(int icorner, double *lo, double *hi, double *pt)
{
  if (icorner % 2) pt[0] = hi[0];
  else pt[0] = lo[0];
  if (icorner / 2) pt[1] = hi[1];
  else pt[1] = lo[1];
  pt[2] = 0.0;
}

/* ----------------------------------------------------------------------
   return coords of a corner point in a 3d hex
   icorner pts 1 to 8 are ordered by x, then by y, then by z
------------------------------------------------------------------------- */

void Surf::hex_corner_point(int icorner, double *lo, double *hi, double *pt)
{
  if (icorner % 2) pt[0] = hi[0];
  else pt[0] = lo[0];
  if ((icorner/2) % 2) pt[1] = hi[1];
  else pt[1] = lo[1];
  if (icorner / 4) pt[2] = hi[2];
  else pt[2] = lo[2];
}

/* ----------------------------------------------------------------------
   return length of line M
------------------------------------------------------------------------- */

double Surf::line_size(int m)
{
  double delta[3];
  MathExtra::sub3(pts[lines[m].p2].x,pts[lines[m].p1].x,delta);
  return MathExtra::len3(delta);
}

/* ----------------------------------------------------------------------
   return area associated with rotating axisymmetric line M around y=0 axis
------------------------------------------------------------------------- */

double Surf::axi_line_size(int m)
{
  double *x1 = pts[lines[m].p1].x;
  double *x2 = pts[lines[m].p2].x;
  double h = x2[0]-x1[0];
  double r = x2[1]-x1[1];
  double area = MY_PI*(x1[1]+x2[1])*sqrt(r*r+h*h);
  return area;
}

/* ----------------------------------------------------------------------
   compute side length and area of a triangle
   return len = length of shortest edge of triangle M
   return area = area of triangle M
------------------------------------------------------------------------- */

double Surf::tri_size(int m, double &len)
{
  double delta12[3],delta13[3],delta23[3],cross[3];

  MathExtra::sub3(pts[tris[m].p2].x,pts[tris[m].p1].x,delta12);
  MathExtra::sub3(pts[tris[m].p3].x,pts[tris[m].p1].x,delta13);
  MathExtra::sub3(pts[tris[m].p3].x,pts[tris[m].p2].x,delta23);
  len = MIN(MathExtra::len3(delta12),MathExtra::len3(delta13));
  len = MIN(len,MathExtra::len3(delta23));

  MathExtra::cross3(delta12,delta13,cross);
  double area = 0.5 * MathExtra::len3(cross);
  return area;
}

/* ----------------------------------------------------------------------
   group surf command called via input script
------------------------------------------------------------------------- */

void Surf::group(int narg, char **arg)
{
  if (narg < 3) error->all(FLERR,"Illegal group command");
}

/* ----------------------------------------------------------------------
   add a new surface group ID, assumed to be unique
------------------------------------------------------------------------- */

int Surf::add_group(const char *id)
{
  if (ngroup == MAXGROUP) 
    error->all(FLERR,"Cannot have more than 32 surface groups");

  int n = strlen(id) + 1;
  gnames[ngroup] = new char[n];
  strcpy(gnames[ngroup],id);
  ngroup++;
  return ngroup-1;
}

/* ----------------------------------------------------------------------
   find a surface group ID
   return index of group or -1 if not found
------------------------------------------------------------------------- */

int Surf::find_group(const char *id)
{
  int igroup;
  for (igroup = 0; igroup < ngroup; igroup++)
    if (strcmp(id,gnames[igroup]) == 0) break;
  if (igroup == ngroup) return -1;
  return igroup;
}

/* ----------------------------------------------------------------------
   add a surface collision model
------------------------------------------------------------------------- */

void Surf::add_collide(int narg, char **arg)
{
  if (narg < 2) error->all(FLERR,"Illegal surf_collide command");

  // error check

  for (int i = 0; i < nsc; i++)
    if (strcmp(arg[0],sc[i]->id) == 0)
      error->all(FLERR,"Reuse of surf_collide ID");

  // extend SurfCollide list if necessary

  if (nsc == maxsc) {
    maxsc += DELTA;
    sc = (SurfCollide **)
      memory->srealloc(sc,maxsc*sizeof(SurfCollide *),"surf:sc");
  }

  // create new SurfCollide class

  if (0) return;

#define SURF_COLLIDE_CLASS
#define SurfCollideStyle(key,Class) \
  else if (strcmp(arg[1],#key) == 0) \
    sc[nsc] = new Class(sparta,narg,arg);
#include "style_surf_collide.h"
#undef SurfCollideStyle
#undef SURF_COLLIDE_CLASS

  else error->all(FLERR,"Invalid surf_collide style");

  nsc++;
}

/* ----------------------------------------------------------------------
   find a surface collide model by ID
   return index of surf collide model or -1 if not found
------------------------------------------------------------------------- */

int Surf::find_collide(const char *id)
{
  int isc;
  for (isc = 0; isc < nsc; isc++)
    if (strcmp(id,sc[isc]->id) == 0) break;
  if (isc == nsc) return -1;
  return isc;
}

/* ----------------------------------------------------------------------
   add a surface reaction model
------------------------------------------------------------------------- */

void Surf::add_react(int narg, char **arg)
{
  if (narg < 2) error->all(FLERR,"Illegal surf_react command");

  // error check

  for (int i = 0; i < nsr; i++)
    if (strcmp(arg[0],sr[i]->id) == 0)
      error->all(FLERR,"Reuse of surf_react ID");

  // extend SurfReact list if necessary

  if (nsr == maxsr) {
    maxsr += DELTA;
    sr = (SurfReact **)
      memory->srealloc(sr,maxsr*sizeof(SurfReact *),"surf:sr");
  }

  // create new SurfReact class

  if (0) return;

#define SURF_REACT_CLASS
#define SurfReactStyle(key,Class) \
  else if (strcmp(arg[1],#key) == 0) \
    sr[nsr] = new Class(sparta,narg,arg);
#include "style_surf_react.h"
#undef SurfReactStyle
#undef SURF_REACT_CLASS

  else error->all(FLERR,"Invalid surf_react style");

  nsr++;
}

/* ----------------------------------------------------------------------
   find a surface reaction model by ID
   return index of surf reaction model or -1 if not found
------------------------------------------------------------------------- */

int Surf::find_react(const char *id)
{
  int isr;
  for (isr = 0; isr < nsr; isr++)
    if (strcmp(id,sr[isr]->id) == 0) break;
  if (isr == nsr) return -1;
  return isr;
}

/* ----------------------------------------------------------------------
   comm of vector of local tallies across all procs
   nrow = # of entries in input vector
   l2g = global surf index of each entry in input vector
   in = input vector
   instride = stride between entries in input vector
   return out = summed tallies for nlocal surfs I own
------------------------------------------------------------------------- */

void Surf::collate_vector(int nrow, int *l2g, 
                          double *in, int instride, double *out)
{
  //if (tally_comm == TALLYAUTO) 
    collate_vector_allreduce(nrow,l2g,in,instride,out);
  //else
  //  collate_vector_irregular(nrow,l2g,in,instride,out);
}

void Surf::collate_vector_allreduce(int nrow, int *l2g, 
                                    double *in, int instride, double *out)
{
  int i,j,m,n;
  double *vec1,*vec2;

  int nglobal;
  if (domain->dimension == 2) nglobal = nline;
  else nglobal = ntri;
  if (nglobal == 0) return;

  double *one,*all;
  memory->create(one,nglobal,"surf:one");
  memory->create(all,nglobal,"surf:all");

  for (i = 0; i < nglobal; i++) one[i] = 0.0;

  m = 0;
  for (i = 0; i < nrow; i++) {
    one[l2g[i]] = in[m];
    m += instride;
  }

  MPI_Allreduce(one,all,nglobal,MPI_DOUBLE,MPI_SUM,world);

  for (i = 0; i < nlocal; i++) out[i] = all[mysurfs[i]];

  // NOTE: don't need to destroy them ?

  memory->destroy(one);
  memory->destroy(all);
}

void Surf::collate_vector_irregular(int nrow, int *l2g, 
                                    double *in, int instride, double *out)
{
}

/* ----------------------------------------------------------------------
   comm of array of local tallies across all procs
   nrow,ncol = # of entries and columns in input array
   l2g = global surf index of each entry in input vector
   in = input vector
   instride = stride between entries in input vector
   return out = summed tallies for nlocal surfs I own
------------------------------------------------------------------------- */

void Surf::collate_array(int nrow, int ncol, int *l2g, 
                         double **in, double **out)
{
  //if (tally_comm == TALLYAUTO) 
    collate_array_allreduce(nrow,ncol,l2g,in,out);
    //else
    // collate_array_irregular(nrow,ncol,l2g,in,out);
}

void Surf::collate_array_allreduce(int nrow, int ncol, int *l2g, 
                                   double **in, double **out)
{
  int i,j,m,n;
  double *vec1,*vec2;

  int nglobal;
  if (domain->dimension == 2) nglobal = nline;
  else nglobal = ntri;
  if (nglobal == 0) return;

  double **one,**all;
  memory->create(one,nglobal,ncol,"surf:one");
  memory->create(all,nglobal,ncol,"surf:all");

  for (i = 0; i < nglobal; i++)
    for (j = 0; j < ncol; j++)
      one[i][j] = 0.0;

  for (i = 0; i < nrow; i++) {
    m = l2g[i];
    for (j = 0; j < ncol; j++) 
      one[m][j] = in[i][j];
  }

  MPI_Allreduce(&one[0][0],&all[0][0],nglobal*ncol,MPI_DOUBLE,MPI_SUM,world);

  for (i = 0; i < nlocal; i++) {
    m = mysurfs[i];
    for (j = 0; j < ncol; j++) 
      out[i][j] += all[m][j];
  }
  
  // NOTE: don't need to destroy them

  memory->destroy(one);
  memory->destroy(all);
}

void Surf::collate_array_irregular(int nrow, int ncol, int *l2g, 
                                   double **in, double **out)
{
}

/* ----------------------------------------------------------------------
   proc 0 writes surf geometry to restart file
------------------------------------------------------------------------- */

void Surf::write_restart(FILE *fp)
{
  fwrite(&ngroup,sizeof(int),1,fp);

  int n;
  for (int i = 0; i < ngroup; i++) {
    n = strlen(gnames[i]) + 1;
    fwrite(&n,sizeof(int),1,fp);
    fwrite(gnames[i],sizeof(char),n,fp);
    fwrite(&bitmask[i],sizeof(int),1,fp);
  }

  fwrite(&npoint,sizeof(int),1,fp);
  fwrite(pts,sizeof(Point),npoint,fp);

  if (domain->dimension == 2) {
    fwrite(&nline,sizeof(int),1,fp);
    for (int i = 0; i < nline; i++) {
      fwrite(&lines[i].type,sizeof(int),1,fp);
      fwrite(&lines[i].mask,sizeof(int),1,fp);
      fwrite(&lines[i].p1,sizeof(int),2,fp);
    }
  }
  if (domain->dimension == 3) {
    fwrite(&ntri,sizeof(int),1,fp);
    for (int i = 0; i < ntri; i++) {
      fwrite(&tris[i].type,sizeof(int),1,fp);
      fwrite(&tris[i].mask,sizeof(int),1,fp);
      fwrite(&tris[i].p1,sizeof(int),3,fp);
    }
  }
}

/* ----------------------------------------------------------------------
   proc 0 reads surf geometry from restart file
   bcast to other procs
------------------------------------------------------------------------- */

void Surf::read_restart(FILE *fp)
{
  int me = comm->me;

  if (me == 0) fread(&ngroup,sizeof(int),1,fp);
  MPI_Bcast(&ngroup,1,MPI_INT,0,world);
  gnames = (char **) memory->smalloc(ngroup*sizeof(char *),"surf:gnames");
  bitmask = (int *) memory->smalloc(ngroup*sizeof(int),"surf:bitmaxk");

  int n;
  for (int i = 0; i < ngroup; i++) {
    if (me == 0) fread(&n,sizeof(int),1,fp);
    MPI_Bcast(&n,1,MPI_INT,0,world);
    gnames[i] = new char[n];
    if (me == 0) fread(gnames[i],sizeof(char),n,fp);
    MPI_Bcast(gnames[i],n,MPI_CHAR,0,world);
    if (me == 0) fread(&bitmask[i],sizeof(int),1,fp);
    MPI_Bcast(&bitmask[i],1,MPI_INT,0,world);
  }

  if (me == 0) fread(&npoint,sizeof(int),1,fp);
  MPI_Bcast(&npoint,1,MPI_INT,0,world);
  pts = (Point *) memory->smalloc(npoint*sizeof(Point),"surf:pts");
  if (me == 0) fread(pts,sizeof(Point),npoint,fp);
  MPI_Bcast(pts,npoint*sizeof(Point),MPI_CHAR,0,world);

  if (domain->dimension == 2) {
    if (me == 0) fread(&nline,sizeof(int),1,fp);
    MPI_Bcast(&nline,1,MPI_INT,0,world);
    lines = (Line *) memory->smalloc(nline*sizeof(Line),"surf:lines");

    if (me == 0) {
      for (int i = 0; i < nline; i++) {
        fread(&lines[i].type,sizeof(int),1,fp);
        fread(&lines[i].mask,sizeof(int),1,fp);
        lines[i].isc = lines[i].isr = -1;
        fread(&lines[i].p1,sizeof(int),2,fp);
        lines[i].norm[0] = lines[i].norm[2] = lines[i].norm[2] = 0.0;
      }
    }
    MPI_Bcast(lines,nline*sizeof(Line),MPI_CHAR,0,world);
  }

  if (domain->dimension == 3) {
    if (me == 0) fread(&ntri,sizeof(int),1,fp);
    MPI_Bcast(&ntri,1,MPI_INT,0,world);
    tris = (Tri *) memory->smalloc(ntri*sizeof(Tri),"surf:tris");

    if (me == 0) {
      for (int i = 0; i < ntri; i++) {
        fread(&tris[i].type,sizeof(int),1,fp);
        fread(&tris[i].mask,sizeof(int),1,fp);
        tris[i].isc = tris[i].isr = -1;
        fread(&tris[i].p1,sizeof(int),3,fp);
        tris[i].norm[0] = tris[i].norm[2] = tris[i].norm[2] = 0.0;
      }
    }
    MPI_Bcast(tris,ntri*sizeof(Tri),MPI_CHAR,0,world);
  }
}

/* ---------------------------------------------------------------------- */

bigint Surf::memory_usage()
{
  bigint bytes = 0;
  bytes += (bigint) npoint * sizeof(Point);
  bytes += (bigint) nline * sizeof(Line);
  bytes += (bigint) ntri * sizeof(Tri);
  bytes += nlocal * sizeof(int);
  return bytes;
}

