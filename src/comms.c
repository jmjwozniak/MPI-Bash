/*
 * comms.c
 *
 *  Created on: Oct 31, 2018
 *      Author: wozniak
 */

#include <stdlib.h>
#include <stdio.h>

#include "comms.h"

/**
   Comm id 0 is MPI_COMM_WORLD
 */
int mpibash_comm_max = 16;
MPI_Comm *mpibash_comms = NULL;
MPI_Comm mpibash_comm_current;

bool
mpibash_comm_init()
{
  mpibash_comm_current = MPI_COMM_WORLD;
  mpibash_comms = calloc(mpibash_comm_max, sizeof(MPI_Comm));
  if (mpibash_comms == NULL)
    return false;
  return true;
}

bool
mpibash_comm_check(intmax_t id)
{
  return (mpibash_comms[id] != 0); // Works for MPICH
}

int
mpibash_comm_store(MPI_Comm comm, intmax_t *id_out)
{
  intmax_t id;
  for (id = 1; id < mpibash_comm_max; id++)
    if (!mpibash_comm_check(id))
      goto found;

  // Did not find an empty slot
  printf("Used too many communicators!\n");
  *id_out = -1;
  return false;

  found:
  mpibash_comms[id] = comm;
  *id_out = id;
  return true;
}

