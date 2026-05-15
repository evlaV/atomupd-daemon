/*
 * Copyright © 2026 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Mock implementation of systemd-time-wait-sync
 *
 * G_TEST_TIME_WAIT_SYNC_RESULT is used to make the script either reach the timeout
 * set by the atomupd-manager, or return an error.
 * G_TEST_TIME_WAIT_SYNC_FLAG_FILE is used to create a flag file to detect multiple
 * invocations.
 */
int
main(int argc, char **argv)
{
   const char *result = getenv("G_TEST_TIME_WAIT_SYNC_RESULT");
   const char *flag_file = getenv("G_TEST_TIME_WAIT_SYNC_FLAG_FILE");

   if (flag_file != NULL) {
      FILE *f = fopen(flag_file, "r");
      if (f != NULL) {
         fclose(f);
         fprintf(stderr, "mock-time-wait-sync: called more than once!\n");
         abort();
      }

      f = fopen(flag_file, "w");
      if (f == NULL)
         abort();

      fclose(f);
   }

   if (result != NULL) {
      if (strcmp(result, "timeout") == 0)
         sleep(30);
      else if (strcmp(result, "failure") == 0)
         return EXIT_FAILURE;
   }

   return EXIT_SUCCESS;
}
