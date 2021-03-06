#include "os.h"
#include "core.h"
#include "stdio.h"

#if OS_UNIX
#include <unistd.h>
#endif

a_i8 *os_read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);
  }

  fseek(file, 0L, SEEK_END);
  size_t fileSize = ftell(file);
  rewind(file);

  a_i8 *buffer = a_i8_empty(fileSize + 1);

  if (buffer == NULL) {
    fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
  }

  size_t bytesRead = fread(buffer->data, sizeof(char), fileSize, file);

  if (bytesRead < fileSize) {
    fprintf(stderr, "Could not read file \"%s\".\n", path);
    exit(74);
  }

  buffer->data[bytesRead] = '\0';

  fclose(file);
  return buffer;
}

#define BUFFER_MAX 1024
a_i8 *os_read_line() {
  a_i8 *result = a_i8_empty(BUFFER_MAX);

  if (fgets((char *)result->data, BUFFER_MAX, stdin)) {
    return result;
  }

  return NULL;
}

a_i8 *os_pwd() {
#if OS_UNIX
  a_i8 *result = a_i8_empty(BUFFER_MAX);
  getcwd((char *)result->data, BUFFER_MAX);

  return result;
#endif
}
