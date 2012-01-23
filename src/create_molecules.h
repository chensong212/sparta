/* ----------------------------------------------------------------------
   DSMC - Sandia parallel DSMC code
   www.sandia.gov/~sjplimp/dsmc.html
   Steve Plimpton, sjplimp@sandia.gov, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2011) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level DSMC directory.
------------------------------------------------------------------------- */

#ifdef COMMAND_CLASS

CommandStyle(create_molecules,CreateMolecules)

#else

#ifndef DSMC_CREATE_MOLECULES_H
#define DSMC_CREATE_MOLECULES_H

#include "pointers.h"

namespace DSMC_NS {

class CreateMolecules : protected Pointers {
 public:
  CreateMolecules(class DSMC *);
  void command(int, char **);

 private:
  int imix;

  void create_local(bigint);
};

}

#endif
#endif