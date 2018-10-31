/*
 * comms.h
 *
 *  Created on: Oct 31, 2018
 *      Author: wozniak
 */

#pragma once

#include <stdbool.h>

#include <mpi.h>

extern int mpibash_comm_max;
extern MPI_Comm *mpibash_comms;
extern MPI_Comm mpibash_comm_current;

bool mpibash_comm_init(void);

/**
   Return true if the id is assigned to a valid communicator, else false.
 */
bool mpibash_comm_check(intmax_t id);

int mpibash_comm_store(MPI_Comm comm, intmax_t *id_out);
