/***********************************
 * MPI-Bash collective functions   *
 *                                 *
 * By Scott Pakin <pakin@lanl.gov> *
 ***********************************/

#include <assert.h>

#include "mpibash.h"

#include "comms.h"
#include "util.h"

#include "MPIX_Comm_launch.h"

/* Synchronize all of the MPI ranks. */
static int
mpi_barrier_builtin (WORD_LIST * list)
{
  no_args(list);
  MPI_TRY(MPI_Barrier(MPI_COMM_WORLD));
  return EXECUTION_SUCCESS;
}

/* Define the documentation for the mpi_barrier builtin. */
static char *mpi_barrier_doc[] = {
  "Synchronizes all of the processes in the MPI job.",
  "",
  "No process will return from mpi_barrier until all processes have",
  "called mpi_barrier.",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_barrier builtin. */
DEFINE_BUILTIN(mpi_barrier, "mpi_barrier");

/* Broadcast a message from one rank to all the others. */
static int
mpi_bcast_builtin (WORD_LIST *list)
{
  char *word;                   /* One argument */
  int root;                     /* MPI root rank */
  char *root_message;           /* Message to broadcast */
  int msglen;                   /* Length in bytes of the above (including the NULL byte) */
  char *varname;                /* Name of the variable to bind the results to */
  static int *all_lengths = NULL;       /* List of every rank's msglen */
  static char *message = NULL;  /* Message received from the root */
  static int alloced = 0;       /* Bytes allocated for the above */
  int i;

  /* Parse the optional message and target variable, which must not be
   * read-only. */
  YES_ARGS(list);
  if (list->next == NULL) {
    /* Non-root */
    root_message = NULL;
    msglen = -1;
  }
  else {
    /* Root */
    root_message = list->word->word;
    msglen = (int) strlen(root_message) + 1;
    list = list->next;
  }
  varname = list->word->word;
  REQUIRE_WRITABLE(varname);
  list = list->next;
  no_args(list);

  /* Acquire global agreement on the root and the message size. */
  if (all_lengths == NULL)
    all_lengths = xmalloc(mpibash_num_ranks * sizeof(int));
  MPI_TRY(MPI_Allgather(&msglen, 1, MPI_INT, all_lengths, 1, MPI_INT, MPI_COMM_WORLD));
  root = -1;
  for (i = 0; i < mpibash_num_ranks; i++) {
    if (all_lengths[i] == -1)
      continue;
    if (root != -1) {
      builtin_error(_
                    ("mpi_bcast: more than one process specified a message"));
      return (EXECUTION_FAILURE);
    }
    root = i;
    msglen = all_lengths[i];
  }
  if (root == -1) {
    builtin_error(_("mpi_bcast: no process specified a message"));
    return (EXECUTION_FAILURE);
  }

  /* Broadcast the message. */
  if (mpibash_rank == root) {
    MPI_TRY(MPI_Bcast(root_message, msglen, MPI_BYTE, root, MPI_COMM_WORLD));
    bind_variable(varname, root_message, 0);
  }
  else {
    if (alloced < msglen) {
      message = xrealloc(message, msglen);
      alloced = msglen;
    }
    MPI_TRY(MPI_Bcast(message, msglen, MPI_BYTE, root, MPI_COMM_WORLD));
    bind_variable(varname, message, 0);
  }
  return EXECUTION_SUCCESS;
}

/* Define the documentation for the mpi_bcast builtin. */
static char *mpi_bcast_doc[] = {
  "Broadcast a message to all processes in the same MPI job.",
  "",
  "Arguments:",
  "  MESSAGE       String to broadcast from one process to all the others.",
  "",
  "  NAME          Scalar variable in which to receive the broadcast message.",
  "",
  "Exactly one process in the MPI job must specify a message to",
  "broadcast.  No process will return from mpi_bcast until all processes",
  "have called mpi_bcast.",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_bcast builtin. */
DEFINE_BUILTIN(mpi_bcast, "mpi_bcast [message] name");

/* Define a reduction-type function (allreduce, scan, exscan, etc.). */
typedef int (*reduction_func_t)(void *, void *, int, MPI_Datatype, MPI_Op, MPI_Comm);

/* Parse an operation name into an MPI_Op.  Return 1 on success, 0 on
 * failure. */
static int
parse_operation (char *name, MPI_Op *op)
{
  /* Define a mapping from operator names to MPI_Op values. */
  typedef struct {
    char *name;        /* Operation name (e.g., "sum") */
    MPI_Op value;      /* Operation value (e.g., MPI_SUM) */
  } opname2value_t;
  static opname2value_t oplist[] = {
    {"max",    MPI_MAX},
    {"min",    MPI_MIN},
    {"sum",    MPI_SUM},
    {"prod",   MPI_PROD},
    {"land",   MPI_LAND},
    {"band",   MPI_BAND},
    {"lor",    MPI_LOR},
    {"bor",    MPI_BOR},
    {"lxor",   MPI_LXOR},
    {"bxor",   MPI_BXOR},
    {"maxloc", MPI_MAXLOC},
    {"minloc", MPI_MINLOC}
  };
  size_t i;

  for (i = 0; i < sizeof(oplist)/sizeof(opname2value_t); i++)
    if (!strcmp(name, oplist[i].name))
      {
        *op = oplist[i].value;
        if (i > 0)
          {
            /* As a performance optimization, bubble up the value we
             * just found. */
            opname2value_t prev = oplist[i - 1];
            oplist[i - 1] = oplist[i];
            oplist[i] = prev;
          }
        return 1;
      }
  return 0;
}

/* Perform any reduction-type operation (allreduce, scan, exscan, etc.). */
static int
reduction_like (WORD_LIST *list, char *funcname, reduction_func_t func)
{
  char *word;                   /* One argument */
  struct {
    long int value;             /* Reduced value */
    int rank;                   /* Rank associated with the above */
  } number, result;
  MPI_Op operation = MPI_SUM;   /* Operation to perform */
  char *varname;                /* Name of the variable to bind the results to */
  intmax_t n;
  int i;

  /* Parse "-O OPERATION" (optional), where OPERATION is a reduction
   * operation. */
  YES_ARGS(list);
  word = list->word->word;
  if (ISOPTION(word, 'O')) {
    list = list->next;
    if (list == 0) {
      sh_needarg(funcname);
      return EX_USAGE;
    }
    word = list->word->word;
    if (!parse_operation(word, &operation)) {
      sh_invalidopt("-O");
      return EX_USAGE;
    }
    list = list->next;
  }

  /* Parse the argument, which must be a number. */
  YES_ARGS(list);
  word = list->word->word;
  if (!legal_number(word, &n)) {
    sh_neednumarg(funcname);
    return EX_USAGE;
  }
  number.value = (long int) n;
  number.rank = mpibash_rank;
  list = list->next;

  /* Parse the target variable, which must not be read-only. */
  YES_ARGS(list);
  varname = list->word->word;
  if (mpibash_rank != 0 || func != MPI_Exscan)
    REQUIRE_WRITABLE(varname);
  list = list->next;
  no_args(list);

  /* Perform the reduction operation.  Bind the given array variable
   * to the result and, for minloc/maxloc, the associated rank. */
  if (mpibash_rank != 0 || func != MPI_Exscan) {
    bind_array_variable(varname, 0, "", 0);
    bind_array_variable(varname, 1, "", 0);
  }
  if (operation == MPI_MINLOC || operation == MPI_MAXLOC) {
    MPI_TRY(func(&number, &result, 1, MPI_LONG_INT, operation, MPI_COMM_WORLD));
    if (mpibash_rank != 0 || func != MPI_Exscan)
      mpibash_bind_array_variable_number(varname, 1, result.rank, 0);
  }
  else
    MPI_TRY(func(&number.value, &result.value, 1, MPI_LONG, operation, MPI_COMM_WORLD));
  if (mpibash_rank != 0 || func != MPI_Exscan)
    mpibash_bind_array_variable_number(varname, 0, result.value, 0);
  return EXECUTION_SUCCESS;
}

/* Perform an inclusive-scan operation. */
static int
mpi_scan_builtin (WORD_LIST *list)
{
  return reduction_like(list, "mpi_scan", MPI_Scan);
}

/* Define the documentation for the mpi_scan builtin. */
static char *mpi_scan_doc[] = {
  "Perform an inclusive scan across all processes in the same MPI job.",
  "",
  "  -O OPERATION  Operation to perform.  Must be one of \"max\", \"min\",",
  "                \"sum\", \"prod\", \"land\", \"band\", \"lor\", \"bor\", \"lxor\",",
  "                \"bxor\", \"maxloc\", or \"minloc\" (default: \"sum\").",
  "",
  "Arguments:",
  "  NUMBER        Integer to use in the scan operation.",
  "",
  "  NAME          Array variable in which to receive the result and, in",
  "                the case of maxloc and minloc, the associated rank.",
  "",
  "In an inclusive-scan operation, each process i presents a number,",
  "a[i].  Once all processes in the MPI job have presented their number,",
  "the command returns a[0] to rank 0, a[0]+a[1] to rank 1,",
  "a[0]+a[1]+a[2] to rank 2, and so forth.  The -O option enables \"+\" to",
  "be replaced with other operations.",
  "",
  "Inclusive scans can be useful for assigning a unique index to each",
  "process in the MPI job.",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_scan builtin. */
DEFINE_BUILTIN(mpi_scan, "mpi_scan [-O operation] number name");

/* Perform an exclusive-scan operation. */
static int
mpi_exscan_builtin (WORD_LIST *list)
{
  return reduction_like(list, "mpi_exscan", MPI_Exscan);
}

/* Define the documentation for the mpi_exscan builtin. */
static char *mpi_exscan_doc[] = {
  "Perform an exclusive scan across all processes in the same MPI job.",
  "",
  "  -O OPERATION  Operation to perform.  Must be one of \"max\", \"min\",",
  "                \"sum\", \"prod\", \"land\", \"band\", \"lor\", \"bor\", \"lxor\",",
  "                \"bxor\", \"maxloc\", or \"minloc\" (default: \"sum\").",
  "",
  "Arguments:",
  "  NUMBER        Integer to use in the scan operation.",
  "",
  "  NAME          Array variable in which to receive the result and, in",
  "                the case of maxloc and minloc, the associated rank.",
  "",
  "In a exclusive-scan operation, each process i presents a number, a[i].",
  "Once all processes in the MPI job have presented their number, the",
  "command assigns a[0] to NAME on rank 1, a[0]+a[1] to NAME on rank 2,",
  "a[0]+a[1]+a[2] to NAME on rank 3, and so forth.  No assignment is",
  "performed on rank 0.  The -O option enables \"+\" to be replaced with",
  "other operations.",
  "",
  "Exclusive scans can be useful for assigning a unique index to each",
  "process in the MPI job.",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_exscan builtin. */
DEFINE_BUILTIN(mpi_exscan, "mpi_exscan [-O operation] number name");

/* Perform an all-reduce operation. */
static int
mpi_allreduce_builtin (WORD_LIST *list)
{
  return reduction_like(list, "mpi_allreduce", MPI_Allreduce);
}

/* Define the documentation for the mpi_allreduce builtin. */
static char *mpi_allreduce_doc[] = {
  "Reduce numbers from all processes in an MPI job to a single number.",
  "",
  "Options:",
  "",
  "  -O OPERATION  Operation to perform.  Must be one of \"max\", \"min\",",
  "                \"sum\", \"prod\", \"land\", \"band\", \"lor\", \"bor\", \"lxor\",",
  "                \"bxor\", \"maxloc\", or \"minloc\" (default: \"sum\").",
  "",
  "Arguments:",
  "  NUMBER        Integer to use in the allreduce operation.",
  "",
  "  NAME          Array variable in which to receive the result and, in",
  "                the case of maxloc and minloc, the associated rank.",
  "",
  "In an all-reduce operation, each process i presents a number, a[i].",
  "Once all processes in the MPI job have presented their number, the",
  "command returns a[0]+a[1]+...+a[n-1] to all ranks.  The -O option",
  "enables \"+\" to be replaced with other operations.",
  "",
  "All-reduces can be useful for reaching global agreement (e.g., of a",
  "termination condition).",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_allreduce builtin. */
DEFINE_BUILTIN(mpi_allreduce, "mpi_allreduce [-O operation] number name");

static int
mpi_comm_set_builtin (WORD_LIST * list)
{
  char *word;
  intmax_t id;

  YES_ARGS(list);
  word = list->word->word;
  if (!legal_number(word, &id)) {
    builtin_error(_("mpi_comm_set: numeric comm-id required"));
    return EX_USAGE;
  }
  list = list->next;
  no_args(list);

  if (!mpibash_comm_check(id)) {
    builtin_error(_("mpi_comm_set: given id does not exist"));
    return EX_USAGE;
  }
  mpibash_comm_current = mpibash_comms[id];
  MPI_Comm_rank (mpibash_comm_current, &mpibash_rank);
  MPI_Comm_size (mpibash_comm_current, &mpibash_num_ranks);


  return EXECUTION_SUCCESS;
}

/* Define the documentation for the mpi_comm_dup builtin. */
static char *mpi_comm_set_doc[] = {
  "Sets the current communicator.",
  "",
  "",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_comm_set builtin. */
DEFINE_BUILTIN(mpi_comm_set, "mpi_comm_set comm");

/* Call MPI_Comm_dup. */
static int
mpi_comm_dup_builtin (WORD_LIST * list)
{
  YES_ARGS(list);
  char *varname = list->word->word;
  REQUIRE_WRITABLE(varname);
  list = list->next;
  no_args(list);

  MPI_Comm newcomm;
  MPI_TRY(MPI_Comm_dup(MPI_COMM_WORLD, &newcomm));

  intmax_t id;
  mpibash_comm_store(newcomm, &id);

  mpibash_bind_variable_number(varname, id, 0);

  return EXECUTION_SUCCESS;
}

/* Define the documentation for the mpi_comm_dup builtin. */
static char *mpi_comm_dup_doc[] = {
  "Does an MPI_Comm_dup.",
  "Returns new comm in given variable name.",
  "",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_barrier builtin. */
DEFINE_BUILTIN(mpi_comm_dup, "mpi_comm_dup name");

/* Call MPI_Comm_dup. */
static int
mpi_comm_split_builtin (WORD_LIST * list)
{
  char *word;
  intmax_t color, key;

  YES_ARGS(list);
  word = list->word->word;
  if (!legal_number(word, &color)) {
    builtin_error(_("mpi_send: numeric color required"));
    return EX_USAGE;
  }
  list = list->next;

  YES_ARGS(list);
  word = list->word->word;
  if (!legal_number(word, &key)) {
    builtin_error(_("mpi_send: numeric key required"));
    return EX_USAGE;
  }
  list = list->next;

  YES_ARGS(list);
  char *varname = list->word->word;
  REQUIRE_WRITABLE(varname);
  list = list->next;
  no_args(list);

  MPI_Comm newcomm;
  MPI_TRY(MPI_Comm_split(mpibash_comm_current, color, key, &newcomm));

  intmax_t id;
  mpibash_comm_store(newcomm, &id);

  mpibash_bind_variable_number(varname, id, 0);

  return EXECUTION_SUCCESS;
}

/* Define the documentation for the mpi_comm_dup builtin. */
static char *mpi_comm_split_doc[] = {
  "Does an MPI_Comm_split.",
  "Returns new comm in given variable name.",
  "",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_comm_split builtin. */
DEFINE_BUILTIN(mpi_comm_split, "mpi_comm_split color key name");

/* Call MPI_Comm_launch. */
static int
mpi_comm_launch_builtin (WORD_LIST * list)
{
  YES_ARGS(list);
  char* cmd = list->word->word;
  list = list->next;
  char **argv;
  mpibash_word_list_to_array(list, &argv);

  MPI_Info info = MPI_INFO_NULL;
  int exit_code = 0;
  MPI_TRY(MPIX_Comm_launch(cmd, argv,
			   info, 0, mpibash_comm_current,
			   &exit_code));
  if (exit_code != 0) printf("warning: exit code is %i\n", exit_code);
  free(argv);
  return exit_code;
}

/* Define the documentation for the mpi_comm_dup builtin. */
static char *mpi_comm_launch_doc[] = {
  "Does an MPI_Comm_launch.",
  "",
  "",
  "",
  "Exit Status:",
  "Returns 0 unless an invalid option is given or an error occurs.",
  NULL
};

/* Describe the mpi_comm_launch builtin. */
DEFINE_BUILTIN(mpi_comm_launch, "mpi_comm_launch cmd args...");
