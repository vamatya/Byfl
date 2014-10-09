/*
 * Library for parsing Byfl binary output files
 * By Scott Pakin <pakin@lanl.gov>
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <setjmp.h>
#include "bfbin.h"

/* Buffer this many bytes of input data for improved performance. */
#define READ_BUFFER_SIZE (10*1024*1024)

/* Invoke a 0-argument (excluding user data) callback function, but
 * only if non-NULL.  Otherwise, do nothing. */
#define INVOKE_CB_0(FUNC)                               \
  do {                                                  \
    if (state->callback_list->FUNC != NULL)             \
      state->callback_list->FUNC(state->user_data);     \
  }                                                     \
  while (0)

/* Invoke a 1-argument (excluding user data) callback function, but
 * only if non-NULL.  Otherwise, do nothing. */
#define INVOKE_CB_1(FUNC, ARG)                                  \
  do {                                                          \
    if (state->callback_list->FUNC != NULL)                     \
      state->callback_list->FUNC(state->user_data, ARG);        \
  }                                                             \
  while (0)

/* Construct an error message and transfer control to the error handler. */
#define THROW_ERROR(MSG, ...)                                           \
  do {                                                                  \
    if (asprintf(&state->error_message, MSG, __VA_ARGS__) == -1)        \
      state->error_message = "Something went wrong";                    \
    longjmp(state->stack_env, 1);                                       \
  }                                                                     \
  while (0)

/* Define all the internal state needed during input-file parsing. */
typedef struct {
  bfbin_callback_t *callback_list;   /* User-provided list of callbacks */
  void *user_data;                   /* User-provided opaque data */
  FILE *fd;                          /* File descriptor for input file */
  const char *filename;              /* Name of input file */
  uint8_t *read_buffer;              /* Storage for buffering file reads */
  char *error_message;               /* Error message to report */
  jmp_buf stack_env;                 /* Stack context for error handling */
  void *last_value;                  /* Storage for data to pass to a callback */
  size_t value_space;                /* Number of bytes allocated for last_value */
} parse_state_t;

#ifndef HAVE_ASPRINTF
/* Implement asprintf with a crude and unsafe hack. */
static int asprintf (char **strp, const char *fmt, ...)
{
  size_t maxlen = 1048576;   /* Arbitrary, large buffer */
  va_list ap;
  int retval;

  *strp = malloc(maxlen);
  if (*strp == NULL)
    return 0;
  vs_start(ap, fmt);
#ifdef HAVE_VSNPRINTF
  retval = vsnprintf(*strp, maxlen, fmt, ap);
#else
  retval = vsprintf(*strp, fmt, ap);
#endif
  va_end(ap);
  *strp = realloc(*strp, strlen(*strp) + 1);   /* Shrink to fit. */
  return retval;
}
#endif

/* Open the Byfl binary-output file and enable a fair amount of
 * buffering.  Invoke the caller-provided callback function on
 * error. */
static void open_binary_file (parse_state_t *state)
{
  char header[7];    /* The string "BYFLBIN" if given a valid file */

  /* Open the file for reading. */
  state->fd = fopen(state->filename, "rb");
  if (state->fd == NULL)
    THROW_ERROR("Failed to open %s (%s)", state->filename, strerror(errno));

  /* If possible, provide a buffer for reading. */
  state->read_buffer = malloc(READ_BUFFER_SIZE);
  if (!state->read_buffer)
    THROW_ERROR("Failed to allocate %lu bytes of memory (%s)",
                (unsigned long)READ_BUFFER_SIZE, strerror(errno));
  if (setvbuf(state->fd, state->read_buffer, _IOFBF, READ_BUFFER_SIZE)) {
    /* It's not critical if setvbuf fails. */
    free(state->read_buffer);
    state->read_buffer = NULL;
  }

  /* Read and validate the magic header sequence. */
  if (fread(header, sizeof(char), 7, state->fd) != 7)
    THROW_ERROR("Failed to read the file header from %s (%s)",
                state->filename, strerror(errno));
  if (memcmp(header, "BYFLBIN", 7) != 0)
    THROW_ERROR("File %s does not appear to be a Byfl binary-output file",
                state->filename);
}

/* Read a big-endian word of a given size into state->last_value. */
static void read_big_endian (parse_state_t *state, size_t word_size)
{
  size_t i;
  uint64_t result = 0;

  /* Read big-endian data. */
  for (i = 0; i < word_size; i++) {
    uint8_t c;
    if (fread(&c, sizeof(uint8_t), 1, state->fd) != 1) {
      char *syserr = strerror(errno);
      THROW_ERROR("Failed to read a byte from %s at position %ld (%s)",
                  state->filename, ftell(state->fd), syserr);
    }
    result = (result<<8) | c;
  }

  /* Store the result at the given pointer. */
  switch (word_size) {
    case 1:
      *(uint8_t *)state->last_value = (uint8_t)result;
      break;

    case 2:
      *(uint16_t *)state->last_value = (uint16_t)result;
      break;

    case 4:
      *(uint32_t *)state->last_value = (uint32_t)result;
      break;

    case 8:
      *(uint64_t *)state->last_value = (uint64_t)result;
      break;

    default:
      THROW_ERROR("Internal error at %s, line %d", __FILE__, __LINE__);
      break;
  }
}

/* Read a string into state->last_value. */
static void read_string (parse_state_t *state)
{
  uint16_t string_len;

  /* Determine the number of bytes to read. */
  read_big_endian (state, sizeof(uint16_t));
  string_len = *(uint16_t *)state->last_value;

  /* Ensure we have enough space. */
  if (state->value_space < string_len + 1) {
    state->value_space = string_len + 1;
    state->last_value = realloc(state->last_value, state->value_space);
    if (!state->last_value)
      THROW_ERROR("Failed to allocate %lu bytes of memory (%s)",
                  state->value_space, strerror(errno));
  }

  /* Read the string and null-terminate it. */
  if (fread(state->last_value, sizeof(char), string_len, state->fd) != string_len)
    THROW_ERROR("Failed to read a %u-byte string from %s (%s)",
                string_len, state->filename, strerror(errno));
  ((char *)state->last_value)[string_len] = '\0';
}

/* Process a basic Byfl table. */
static void process_byfl_basic_table (parse_state_t *state)
{
  BINOUT_COL_T *columntypes = NULL;   /* List of column types */
  size_t numcols = 0;                 /* Number of valid entries in columntypes */
  size_t cols_alloced = 0;            /* Number of entries allocated for columntypes */
  BINOUT_COL_T coltype;               /* Type of a single column */

  /* Read and parse each column header. */
  INVOKE_CB_0(column_begin_cb);
  do {
    /* Read and store a column header. */
    read_big_endian(state, sizeof(BINOUT_COL_T));
    coltype = *(BINOUT_COL_T *)state->last_value;
    if (coltype != BINOUT_COL_NONE) {
      numcols++;
      if (numcols > cols_alloced) {
        /* Allocate more space. */
        cols_alloced = numcols*2;
        columntypes = realloc(columntypes, cols_alloced*sizeof(BINOUT_COL_T));
        if (!columntypes)
          THROW_ERROR("Failed to allocate %lu bytes of memory (%s)",
                      cols_alloced*sizeof(BINOUT_COL_T), strerror(errno));
      }
      columntypes[numcols - 1] = coltype;
    }

    /* Invoke the appropriate callback. */
    if (coltype != BINOUT_COL_NONE)
      read_string(state);
    switch (coltype) {
      case BINOUT_COL_UINT64:
        INVOKE_CB_1(column_uint64_cb, state->last_value);
        break;

      case BINOUT_COL_STRING:
        INVOKE_CB_1(column_string_cb, state->last_value);
        break;

      case BINOUT_COL_BOOL:
        INVOKE_CB_1(column_bool_cb, state->last_value);
        break;

      case BINOUT_COL_NONE:
        INVOKE_CB_0(column_end_cb);
        break;

      default:
        THROW_ERROR("Internal error at %s, line %d", __FILE__, __LINE__);
        break;
    }
  }
  while (coltype != BINOUT_COL_NONE);

  /* Read and parse each row of data and invoke callback functions. */
  while (1) {
    BINOUT_ROW_T rowtype;               /* Type of a single row */
    size_t i;

    /* Determine if the row contains any data. */
    read_big_endian(state, sizeof(BINOUT_ROW_T));
    rowtype = *(BINOUT_ROW_T *)state->last_value;
    if (rowtype == BINOUT_ROW_NONE)
      break;

    /* Invoke the appropriate callbacks. */
    INVOKE_CB_0(row_begin_cb);
    for (i = 0; i < numcols; i++)
      switch (columntypes[i]) {
        case BINOUT_COL_UINT64:
          read_big_endian(state, sizeof(uint64_t));
          INVOKE_CB_1(data_uint64_cb, *(uint64_t *)state->last_value);
          break;

        case BINOUT_COL_STRING:
          read_string(state);
          INVOKE_CB_1(data_string_cb, state->last_value);
          break;

        case BINOUT_COL_BOOL:
          read_big_endian(state, sizeof(uint8_t));
          INVOKE_CB_1(data_bool_cb, *(uint8_t *)state->last_value);
          break;

        default:
          THROW_ERROR("Internal error at %s, line %d", __FILE__, __LINE__);
          break;
      }
    INVOKE_CB_0(row_end_cb);
  }
}

/* Process a key:value Byfl table. */
static void process_byfl_key_value_table (parse_state_t *state)
{
  while (1) {
    BINOUT_COL_T coltype;          /* Type of the current column */

    /* Read a key type and name. */
    read_big_endian(state, sizeof(BINOUT_COL_T));
    coltype = *(BINOUT_COL_T *)state->last_value;
    if (coltype != BINOUT_COL_NONE)
      break;
    read_string(state);

    /* Invoke the appropriate callback functions. */
    switch (coltype) {
      case BINOUT_COL_UINT64:
        INVOKE_CB_1(column_uint64_cb, state->last_value);
        read_big_endian(state, sizeof(uint64_t));
        INVOKE_CB_1(data_uint64_cb, *(uint64_t *)state->last_value);
        break;

      case BINOUT_COL_STRING:
        INVOKE_CB_1(column_string_cb, state->last_value);
        read_string(state);
        INVOKE_CB_1(data_string_cb, state->last_value);
        break;

      case BINOUT_COL_BOOL:
        INVOKE_CB_1(column_bool_cb, state->last_value);
        read_big_endian(state, sizeof(uint8_t));
        INVOKE_CB_1(data_bool_cb, *(uint8_t *)state->last_value);
        break;

      default:
        THROW_ERROR("Internal error at %s, line %d", __FILE__, __LINE__);
        break;
    }
  }
}

/* Process a complete Byfl table.  Return 1 on success, 0 on EOF. */
static int process_byfl_table (parse_state_t *state)
{
  BINOUT_TABLE_T tabletype;

  /* Read the table type and table name. */
  read_big_endian(state, sizeof(BINOUT_TABLE_T));
  tabletype = *(BINOUT_TABLE_T *)state->last_value;
  if (tabletype == BINOUT_TABLE_NONE)
    return 0;
  read_string(state);

  /* Invoke the appropriate function to parse the table. */
  switch (tabletype) {
    case BINOUT_TABLE_BASIC:
      INVOKE_CB_1(table_basic_cb, state->last_value);
      process_byfl_basic_table(state);
      INVOKE_CB_0(table_end_cb);
      break;

    case BINOUT_TABLE_KEYVAL:
      INVOKE_CB_1(table_keyval_cb, state->last_value);
      process_byfl_keyval_table(state);
      INVOKE_CB_0(table_end_cb);
      break;

    default:
      THROW_ERROR("Internal error at %s, line %d", __FILE__, __LINE__);
      break;
  }
  return 1;
}

/* Process an entire Byfl binary output file.  This is the sole entry
 * point for the library. */
void bf_process_byfl_file (const char *byfl_filename,
                           bfbin_callback_t *callback_list,
                           size_t callback_list_len,
                           void *user_data)
{
  parse_state_t local_state;  /* Local state information for parsing the input file */

  /* Ensure the caller was built against the correct version of the
   * header and library. */
  if (sizeof(callback_list) != callback_list_len) {
    if (callback_list_len > sizeof(void *) && callback_list->error_cb != NULL)
      callback_list->error_cb(user_data, "Mismatched bfbin header and library file");
    return;
  }

  /* Initialize our processing state. */
  memset(&local_state, 0, sizeof(parse_state_t));
  local_state.callback_list = callback_list;
  local_state.user_data = user_data;
  local_state.filename = byfl_filename;

  /* Establish an error handler. */
  if (setjmp(local_state.stack_env) != 0) {
    /* Issue an error message. */
    if (local_state.error_message != NULL) {
      if (local_state.callback_list->error_cb != NULL)
        local_state.callback_list->error_cb(user_data, local_state.error_message);
      free(local_state.error_message);
      local_state.error_message = NULL;
    }

    /* Close the input file. */
    if (local_state.fd != NULL)
      fclose(local_state.fd);
    return;
  }

  /* Allocate memory for storing transient data values read from the file. */
  local_state.value_space = sizeof(uint64_t);
  local_state.last_value = malloc(local_state.value_space);
  if (local_state.last_value == NULL) {
    parse_state_t *state = &local_state;   /* Macro expects the name "state". */
    THROW_ERROR("Failed to allocate %lu bytes of memory (%s)",
                local_state.value_space, strerror(errno));
  }

  /* Open the Byfl binary-output file for input.*/
  open_binary_file(&local_state);

  /* Process each table in turn. */
  while (process_byfl_table(&local_state))
    ;

  /* Close the Byfl binary-output file. */
  fclose(local_state.fd);
}