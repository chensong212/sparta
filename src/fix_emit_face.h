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

#ifdef FIX_CLASS

FixStyle(emit/face,FixEmitFace)

#else

#ifndef SPARTA_FIX_EMIT_FACE_H
#define SPARTA_FIX_EMIT_FACE_H

#include "fix_emit.h"
#include "surf.h"
#include "grid.h"

namespace SPARTA_NS {

class FixEmitFace : public FixEmit {
 public:
  FixEmitFace(class SPARTA *, int, char **);
  ~FixEmitFace();
  void init();
  void post_compress_grid();

 private:
  int imix,np,subsonic,subsonic_style;
  int faces[6];
  int npertask,nthresh;
  double psubsonic,tsubsonic,nsubsonic;
  double tprefactor,sprefactor;

  // copies of data from other classes

  int dimension;
  double fnum,dt;
  int nspecies;
  double nrho,temp_thermal,temp_rot,temp_vib;
  double *vstream,*vscale,*fraction,*cummulative;

  Surf::Point *pts;
  Surf::Line *lines;
  Surf::Tri *tris;

  // one insertion task for a cell and a face

  struct Task {
    double lo[3];               // lower-left corner of face
    double hi[3];               // upper-right corner of face
    double normal[3];           // inward normal from external boundary
    double area;                // area of face;
    double ntarget;             // # of mols to insert for all species
    double *ntargetsp;          // # of mols to insert for each species
    int icell;                  // associated cell index, unsplit or split cell
    int iface;                  // which face of unsplit or split cell
    int pcell;                  // associated cell index for particles
                                // unsplit or sub cell (not split cell)
    int ndim;                   // dim (0,1,2) normal to face
    int pdim,qdim;              // 2 dims (0,1,2) parallel to face
  };

                         // ntask = # of tasks is stored by parent class
  Task *tasks;           // list of particle insertion tasks
  int ntaskmax;          // max # of tasks allocated

  // instantaneous subsonic per-task values

  struct SSTask {
    double nrho,temp_thermal;
    double vstream[3];
  };

  SSTask *sstasks;

  int maxactive;
  int *activecell;

  // persistent subsonic per-task values

  struct SSTaskSave {
    double foo;
  };

  SSTaskSave *sstaskssave;

  // private methods

  int create_task(int);
  void perform_task();

  int split(int, int);

  void subsonic_inflow();
  void subsonic_sort();
  void subsonic_grid();

  int pack_task(int, char *, int);
  int unpack_task(char *, int);
  void copy_task(int, int, int, int);
  void grow_task();

  int option(int, char **);
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running SPARTA to see the offending line.

E: Fix emit/face mixture ID does not exist

Self-explanatory.

E: Cannot use fix emit/face in z dimension for 2d simulation

Self-explanatory.

E: Cannot use fix emit/face in y dimension for axisymmetric

This is because the y dimension boundaries cannot be
emit/face boundaries for an axisymmetric model.

E: Cannot use fix emit/face n > 0 with perspecies yes

This is because the perspecies option calculates the
number of particles to insert itself.

E: Cannot use fix emit/face on periodic boundary

Self-explanatory.

E: Fix emit/face used on outflow boundary

Self-explanatory.

*/
