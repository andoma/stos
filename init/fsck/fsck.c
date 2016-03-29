#include <string.h>
#include <stdio.h>

#include "common.h"
#include "fsck.fat.h"
#include "io.h"
#include "boot.h"
#include "fat.h"
#include "file.h"
#include "check.h"
#include "charconv.h"

int interactive = 0, rw = 1, list = 0, test = 0, verbose = 1, write_immed = 0;
int atari_format = 0, boot_only = 0;
unsigned n_files = 0;
void *mem_queue = NULL;

int
fsck(const char *dev)
{
  DOS_FS fs;

  const int verify = 0;
  const int salvage_files = 0;
  uint32_t free_clusters;

  memset(&fs, 0, sizeof(fs));

  fs_open((char *)dev, rw);

  read_boot(&fs);

  if (verify)
    printf("Starting check/repair pass.\n");
  while (read_fat(&fs), scan_root(&fs))
    qfree(&mem_queue);
  if (test)
    fix_bad(&fs);
  if (salvage_files)
    reclaim_file(&fs);
  else
    reclaim_free(&fs);
  free_clusters = update_free(&fs);
  file_unused();
  qfree(&mem_queue);
  if (verify) {
    n_files = 0;
    printf("Starting verification pass.\n");
    read_fat(&fs);
    scan_root(&fs);
    reclaim_free(&fs);
    qfree(&mem_queue);
  }

  if (fs_changed()) {
    if (rw) {
      printf("Performing changes.\n");
    } else
      printf("Leaving filesystem unchanged.\n");
  }

  if (!boot_only)
    printf("%s: %u files, %lu/%lu clusters\n", dev,
           n_files, (unsigned long)fs.clusters - free_clusters, (unsigned long)fs.clusters);

  return fs_close(rw) ? 1 : 0;
}
