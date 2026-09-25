/*********************************************************
 * Copyright (C) 2014-2019 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * dndXdg.c --
 *
 *      Drag and drop routines for X Desktop Group (XDG) / freedesktop.org
 *      platforms.
 */


#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <pwd.h>
#include <stdlib.h>
#include <glib.h>

#if defined __FreeBSD__
#include <unistd.h>
#endif

#include "dndInt.h"
#include "dnd.h"

#include "err.h"
#include "file.h"
#include "log.h"
#include "posix.h"
#include "str.h"
#include "strutil.h"
#include "su.h"
#include "unicode.h"
#include "vm_atomic.h"


static const char *CreateRealRootDirectory(void);
static const char *CreateApparentRootDirectory(void);
static char *FindSuitableExistingDirectory(const char *realRoot,
                                           const char *apparentRoot);
static void PruneStagingDirectories(const char *realRoot,
                                    const char *apparentRoot);
static char *CreateStagingDirectory(const char *realRoot,
                                    const char *apparentRoot);


/*
 *-----------------------------------------------------------------------------
 *
 * Xdg_GetCacheHome --
 *
 *      Determine path appropriate for "user-specific non-essential (cached)
 *      data"[1].
 *
 *      1. <http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html>
 *
 * Results:
 *      Returns a pointer to a valid path on success or NULL on failure.
 *
 * Side effects:
 *      First call may allocate memory which remains allocated for the life
 *      of the program.
 *
 *      NOT thread-safe.
 *
 * TODO:
 *      Move bora-vmsoft/lib/xdg to bora/lib, then move this to bora/lib/xdg.
 *      Unit tests.  (There is no good home for these at the moment.)
 *
 *-----------------------------------------------------------------------------
 */

const char *
Xdg_GetCacheHome(void)
{
   static char *result = NULL;

   if (result == NULL) {
      do {
         struct passwd *pw;

         if (!Id_IsSetUGid()) {
            const char *base = NULL;

            /*
             * Paranoia:  Avoid environment variables if running in a sensitive
             *            context.  sudo or other loader should've sanitized the
             *            environment, but, well, we're paranoid, remember?
             */

            // 1. $XDG_CACHE_HOME
            base = Posix_Getenv("XDG_CACHE_HOME");
            if (Util_IsAbsolutePath(base)) {
               result = Util_SafeStrdup(base);
               break;
            }

            // 2. $HOME/.cache
            base = Posix_Getenv("HOME");
            if (Util_IsAbsolutePath(base)) {
               result = Util_SafeStrdup(base);
               StrUtil_SafeStrcat(&result, "/.cache");
               break;
            }
         }

         // 3. <pw_dir>/.cache
         pw = Posix_Getpwuid(geteuid());

         if (pw != NULL && Util_IsAbsolutePath(pw->pw_dir)) {
            result = Str_Asprintf(NULL, "%s/.cache", pw->pw_dir);
         }
      } while(0);
   }

   VERIFY(result == NULL || result[0] == '/');
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnD_CreateStagingDirectory --
 *
 *    See dndCommon.c.
 *
 * Results:
 *    A string containing the newly created name, or NULL on failure.
 *
 * Side effects:
 *    A directory is created
 *
 *    Caller is responsible for freeing returned pointer.
 *
 *    NOT thread-safe.
 *
 *-----------------------------------------------------------------------------
 */

char *
DnD_CreateStagingDirectory(void)
{
   const char *realRoot;
   const char *apparentRoot;
   char *existingDir;

   /*
    * On XDG platforms, there are two roots:
    *
    * 1. Per-user real root ($HOME/.cache/vmware/drag_and_drop)
    *
    *    Files are stored here, leaving cleanup to discretion of users and
    *    adminisitrators, and may count against users' storage quotas.  Most
    *    importantly, however, is that it means avoiding tmpfs-backed /tmp
    *    which may be too small for large files.
    *
    * 2. Apparent root - /tmp/VMwareDnD
    *
    *    Path known to vmblock implementations.  Contains only symlinks to
    *    users' real root.
    *
    * Therefore DnD targets may access paths via
    *    /var/run/vmblock (vmblock file system) ->
    *    /tmp/VMwareDnD   (apparent root) ->
    *    $HOME/.cache/vmware/drag_and_drop (real root).
    */


   /*
    * Lookup or create real root.
    *
    * We don't worry about cleaning up or removing this directory in case
    * of failure later in this function.
    */

   realRoot = CreateRealRootDirectory();
   if (realRoot == NULL) {
      return NULL;
   }


   /*
    * Lookup or create apparent root.
    */

   apparentRoot = CreateApparentRootDirectory();
   if (apparentRoot == NULL) {
      return NULL;
   }

   PruneStagingDirectories(realRoot, apparentRoot);


   /*
    * Search real root for empty directory.  If found, (re)use it.
    */

   existingDir = FindSuitableExistingDirectory(realRoot, apparentRoot);
   if (existingDir != NULL) {
      return existingDir;
   }


   /*
    * Generate new temporary directory.
    *    - Attempt to symlink $apparent/XXXXXX to $real/XXXXXX.
    *    - mkdir -p $real/XXXXXX.
    */

   return CreateStagingDirectory(realRoot, apparentRoot);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DetermineRealRootDirectory --
 * CreateRealRootDirectory --
 *
 *      Produces the path of the real staging directory root (e.g.
 *      $HOME/.cache/vmware/drag_and_drop).
 *
 * Results:
 *      !NULL   Success. Points to real staging root path.
 *      NULL    Failure.
 *
 * Side effects:
 *      May create the .cache/vmware/drag_and_drop hierarchy.
 *
 *-----------------------------------------------------------------------------
 */

static const char *
DetermineRealRootDirectory(void)
{
   static char *completePath;
   const char *cachePath;

   if (completePath != NULL) {
      return completePath;
   }

   cachePath = Xdg_GetCacheHome();
   if (cachePath != NULL) {
      const char dndSuffix[] = "/vmware/drag_and_drop/";

      completePath = Unicode_Duplicate(cachePath);
      StrUtil_SafeStrcat(&completePath, dndSuffix);
      VERIFY(strlen(completePath) < PATH_MAX);

      Log_Trivia("dnd: will stage to %s\n", completePath);
      return completePath;
   }

   Log_Trivia("dnd: failed to determine path\n");
   return NULL;
}

static const char *
CreateRealRootDirectory(void)
{
   const char *realRoot = DetermineRealRootDirectory();

   if (   realRoot != NULL
       && (   File_IsDirectory(realRoot)
           || File_CreateDirectoryHierarchyEx(realRoot, 0700, NULL))) {
      return realRoot;
   }

   return NULL;
}


/*
 *----------------------------------------------------------------------------
 *
 * CreateApparentRootDirectory --
 *
 *    Checks if the root staging directory exists with the correct permissions,
 *    or creates it if necessary.
 *
 * Results:
 *    !NULL     Success. String containing apparent root directory path.
 *    NULL      failure.
 *
 * Side effects:
 *    May create apparent root (/tmp/VMwareDnD) and/or change its permissions.
 *
 *----------------------------------------------------------------------------
 */

static const char *
CreateApparentRootDirectory(void)
{
   const char *root;

   /*
    * DnD_GetFileRoot() gives us a pointer to a static string, so there's no
    * need to free anything.
    *
    * XXX On XDG platforms this path ("/tmp/VMwareDnD") is created by an
    *     init script, so we could remove some of the code below and just bail
    *     if the user deletes it.
    */

   root = DnD_GetFileRoot();
   if (!root) {
      return NULL;
   }

   if (File_Exists(root)) {
      if (!DnDRootDirUsable(root)) {
         /*
          * The directory already exists and its permissions are wrong.
          */
         Log_Trivia("dnd: The root dir is not usable.\n");
         return NULL;
      }
   } else {
      if (   !File_CreateDirectory(root)
          || !DnDSetPermissionsOnRootDir(root)) {
         /* We couldn't create the directory or set the permissions. */
         return NULL;
      }
   }

   return root;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FindSuitableExistingDirectory --
 *
 *      Given the pair of <realRoot, apparentRoot>, search for an existing
 *      directory within `realRoot` satisfying the following conditions:
 *
 *         1. it's empty;
 *         2. it's pointed to by a symlink of the same name from apparentRoot.
 *
 * Results:
 *      !NULL   Success.  Caller must free result.
 *      NULL    Failure.
 *
 * Side effects:
 *      May create a symlink.
 *
 *-----------------------------------------------------------------------------
 */

static char *
FindSuitableExistingDirectory(
   const char *realRoot,       // IN: e.g. $HOME/.cache/vmware/drag_and_drop/
   const char *apparentRoot)   // IN: e.g. /tmp/VMwareDnD/
{
   char *result = NULL;

   char **stagingDirList;
   int numStagingDirs = File_ListDirectory(realRoot, &stagingDirList);
   int i;

   for (i = 0; i < numStagingDirs && result == NULL; i++) {
      char *stagingDir = Unicode_Append(realRoot, stagingDirList[i]);
      char *apparentStagingDir = Unicode_Append(apparentRoot, stagingDirList[i]);
      char *temp = NULL;
      struct stat sb;

      if (   File_IsEmptyDirectory(stagingDir)
          && (   Posix_Symlink(stagingDir, apparentStagingDir) == 0
              || (   Posix_Lstat(apparentStagingDir, &sb) == 0
                  && sb.st_uid == getuid()
                  && (temp = Posix_ReadLink(apparentStagingDir)) != NULL
                  && strcmp(stagingDir, temp) == 0))) {
         result = apparentStagingDir;
         apparentStagingDir = NULL;
      }

      free(stagingDir);
      free(apparentStagingDir);
      free(temp);
   }

   Util_FreeStringList(stagingDirList, numStagingDirs);
   return result;
}


/*
 * A staging directory is removed once nothing in it has changed for this
 * long, no process holds a file in it open or has it as its working
 * directory, and neither the clipboard nor a drag in progress uses it. See
 * PruneStagingDirectories(). A file manager copying out of the directory
 * holds the file open, and any change starts the wait again.
 */
#define DND_STAGING_PRUNE_AGE_SEC (2 * 60)

/* Deeper trees are kept, rather than walked further. */
#define DND_STAGING_PRUNE_MAX_DEPTH 32

/* The mkdtemp() template for a staging directory name. */
#define DND_STAGING_NAME_TEMPLATE "XXXXXX"
#define DND_STAGING_NAME_LEN (sizeof DND_STAGING_NAME_TEMPLATE - 1)

/* The staging directory behind the file list the clipboard offers. */
static char *gClipboardStagingDir;

/* The staging directory of a host to guest drag whose files are not done. */
static char *gDragStagingDir;


/*
 *-----------------------------------------------------------------------------
 *
 * DnD_SetClipboardStagingDir --
 *
 *      Record the staging directory whose files the clipboard now offers, so
 *      that it outlives the prune for as long as it may still be pasted.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Replaces the previously recorded directory.
 *
 *-----------------------------------------------------------------------------
 */

static void
RecordStagingDir(char **slot,               // IN/OUT
                 const char *stagingDir)    // IN/OPT
{
   size_t len;

   free(*slot);
   *slot = Util_SafeStrdup(stagingDir);
   /* The prune compares without the trailing slash staging paths carry. */
   len = *slot != NULL ? strlen(*slot) : 0;
   while (len > 1 && (*slot)[len - 1] == '/') {
      (*slot)[--len] = '\0';
   }
}


void
DnD_SetClipboardStagingDir(const char *stagingDir)   // IN/OPT
{
   RecordStagingDir(&gClipboardStagingDir, stagingDir);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnD_SetDragStagingDir --
 *
 *      Record the staging directory of a host to guest drag until its files
 *      are done, so that the prune leaves it alone while the drag is still
 *      over a target. NULL clears it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Replaces the previously recorded directory.
 *
 *-----------------------------------------------------------------------------
 */

void
DnD_SetDragStagingDir(const char *stagingDir)   // IN/OPT
{
   RecordStagingDir(&gDragStagingDir, stagingDir);
}


/*
 *-----------------------------------------------------------------------------
 *
 * IsInUseStagingDir --
 *
 *      Whether the clipboard or a drag in progress uses the staging directory
 *      known by either of these two paths.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
IsInUseStagingDir(const char *apparentDir,   // IN
                  const char *realDir)       // IN
{
   const char *inUse[] = { gClipboardStagingDir, gDragStagingDir };
   size_t i;

   for (i = 0; i < ARRAYSIZE(inUse); i++) {
      if (inUse[i] != NULL &&
          (strcmp(inUse[i], apparentDir) == 0 ||
           strcmp(inUse[i], realDir) == 0)) {
         return TRUE;
      }
   }
   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ChangedSince --
 *
 *      Whether anything under `path` was modified or had its status changed
 *      at or after `cutoff`, not following symlinks. A tree deeper than
 *      DND_STAGING_PRUNE_MAX_DEPTH, or one that cannot be read, counts as
 *      changed, so that it is kept.
 *
 * Results:
 *      TRUE if something changed or the tree could not be read in full,
 *      FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
ChangedSince(const char *path,   // IN
             time_t cutoff,      // IN
             int depth)          // IN
{
   struct stat sb;
   DIR *dir;
   struct dirent *de;
   Bool changed = FALSE;

   if (lstat(path, &sb) != 0) {
      return TRUE;
   }
   if (sb.st_mtime >= cutoff || sb.st_ctime >= cutoff) {
      return TRUE;
   }
   if (!S_ISDIR(sb.st_mode)) {
      return FALSE;
   }
   if (depth >= DND_STAGING_PRUNE_MAX_DEPTH || (dir = opendir(path)) == NULL) {
      return TRUE;
   }
   while (!changed && (de = readdir(dir)) != NULL) {
      char *child;

      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
         continue;
      }
      child = Str_Asprintf(NULL, "%s/%s", path, de->d_name);
      changed = child == NULL || ChangedSince(child, cutoff, depth + 1);
      free(child);
   }
   closedir(dir);

   return changed;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MakeTreeWritable --
 *
 *      Give the owner read, write and search permission on every directory
 *      under `path` that this user owns, down to DND_STAGING_PRUNE_MAX_DEPTH.
 *      The host's permissions can have taken them away, and the tree cannot
 *      be deleted without them.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Changes directory permissions under `path`.
 *
 *-----------------------------------------------------------------------------
 */

static void
MakeTreeWritable(const char *path,   // IN
                 int depth)          // IN
{
   struct stat sb;
   DIR *dir;
   struct dirent *de;

   if (lstat(path, &sb) != 0 || !S_ISDIR(sb.st_mode) ||
       sb.st_uid != getuid() || depth >= DND_STAGING_PRUNE_MAX_DEPTH) {
      return;
   }
   if ((sb.st_mode & S_IRWXU) != S_IRWXU) {
      chmod(path, (sb.st_mode & 07777) | S_IRWXU);
   }
   if ((dir = opendir(path)) == NULL) {
      return;
   }
   while ((de = readdir(dir)) != NULL) {
      char *child;

      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
         continue;
      }
      child = Str_Asprintf(NULL, "%s/%s", path, de->d_name);
      if (child != NULL) {
         MakeTreeWritable(child, depth + 1);
         free(child);
      }
   }
   closedir(dir);
}


/*
 *-----------------------------------------------------------------------------
 *
 * AddLinkTarget --
 *
 *      Append the absolute target of the symlink `linkPath` to `paths`.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May grow `paths`, which stays NULL-terminated.
 *
 *-----------------------------------------------------------------------------
 */

static void
AddLinkTarget(const char *linkPath,   // IN
              char ***paths,          // IN/OUT
              int *count,             // IN/OUT
              int *cap)               // IN/OUT
{
   char target[PATH_MAX];
   ssize_t len = readlink(linkPath, target, sizeof target - 1);

   if (len <= 0 || target[0] != '/') {
      return;
   }
   target[len] = '\0';
   if (*count + 1 >= *cap) {
      *cap = *cap == 0 ? 256 : *cap * 2;
      *paths = Util_SafeRealloc(*paths, *cap * sizeof **paths);
   }
   (*paths)[(*count)++] = Util_SafeStrdup(target);
   (*paths)[*count] = NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CollectOpenPaths --
 *
 *      The resolved paths of every open file and working directory of the
 *      processes this user can inspect. The kernel reports each by its
 *      resolved path, whether it was opened through the staging path, the
 *      apparent root or vmblock.
 *
 * Results:
 *      A NULL-terminated array to free with Util_FreeStringList(), and its
 *      length in *count. NULL if /proc cannot be read, and then nothing
 *      should be removed.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static char **
CollectOpenPaths(int *count)   // OUT
{
   DIR *proc = opendir("/proc");
   struct dirent *pe;
   char **paths = NULL;
   int n = 0;
   int cap = 0;

   *count = 0;
   if (proc == NULL) {
      return NULL;
   }
   while ((pe = readdir(proc)) != NULL) {
      char dirPath[64];
      char linkPath[96];
      DIR *fds;
      struct dirent *fe;

      if (pe->d_name[0] < '0' || pe->d_name[0] > '9') {
         continue;
      }
      Str_Snprintf(linkPath, sizeof linkPath, "/proc/%s/cwd", pe->d_name);
      AddLinkTarget(linkPath, &paths, &n, &cap);

      Str_Snprintf(dirPath, sizeof dirPath, "/proc/%s/fd", pe->d_name);
      if ((fds = opendir(dirPath)) == NULL) {
         continue;
      }
      while ((fe = readdir(fds)) != NULL) {
         if (fe->d_name[0] == '.') {
            continue;
         }
         Str_Snprintf(linkPath, sizeof linkPath, "%s/%s", dirPath, fe->d_name);
         AddLinkTarget(linkPath, &paths, &n, &cap);
      }
      closedir(fds);
   }
   closedir(proc);
   if (paths == NULL) {
      paths = Util_SafeCalloc(1, sizeof *paths);
   }
   *count = n;

   return paths;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PathIsUnder --
 *
 *      Whether `path` is `dir` or lies within it.
 *
 * Results:
 *      TRUE if it is, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
PathIsUnder(const char *path,   // IN
            const char *dir)    // IN
{
   size_t len = strlen(dir);

   return strncmp(path, dir, len) == 0 &&
          (path[len] == '\0' || path[len] == '/');
}


/*
 *-----------------------------------------------------------------------------
 *
 * IsStagingName --
 *
 *      Whether `name` is one mkdtemp() could have made for
 *      CreateStagingDirectory(), DND_STAGING_NAME_LEN characters from
 *      [A-Za-z0-9].
 *
 * Results:
 *      TRUE if it is, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
IsStagingName(const char *name)   // IN
{
   size_t i;

   for (i = 0; i < DND_STAGING_NAME_LEN; i++) {
      char c = name[i];

      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9'))) {
         return FALSE;
      }
   }
   return name[DND_STAGING_NAME_LEN] == '\0';
}


/*
 *-----------------------------------------------------------------------------
 *
 * PruneStagingDirectories --
 *
 *      Remove staging directories whose transfers are over.
 *
 *      Nothing tells the guest when a file manager has finished copying out
 *      of a staging directory, and a paste can be repeated from the same
 *      clipboard long after the first. So a directory goes only when its name
 *      is one this file creates, it is not empty (an empty one is reused by
 *      FindSuitableExistingDirectory(), possibly for a drag still in
 *      progress), neither the clipboard nor a drag in progress uses it (see
 *      DnD_SetClipboardStagingDir() and DnD_SetDragStagingDir()), nothing in
 *      it has changed for DND_STAGING_PRUNE_AGE_SEC, and no process holds a
 *      file in it open or has it as its working directory. The symlink in the apparent root
 *      that points at it goes with it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Deletes directories under realRoot and symlinks under apparentRoot.
 *
 *-----------------------------------------------------------------------------
 */

static void
PruneStagingDirectories(const char *realRoot,       // IN
                        const char *apparentRoot)   // IN
{
   char **names = NULL;
   int count = File_ListDirectory(realRoot, &names);
   time_t cutoff = time(NULL) - DND_STAGING_PRUNE_AGE_SEC;
   char **openPaths = NULL;
   int openCount = 0;
   Bool collected = FALSE;
   int i;

   for (i = 0; i < count; i++) {
      char *realDir = Unicode_Append(realRoot, names[i]);
      char *apparentDir = Unicode_Append(apparentRoot, names[i]);
      char *resolvedDir = NULL;
      char *link = NULL;
      Bool open = FALSE;
      struct stat sb;
      int j;

      if (!IsStagingName(names[i]) ||
          lstat(realDir, &sb) != 0 || !S_ISDIR(sb.st_mode) ||
          sb.st_uid != getuid() || File_IsEmptyDirectory(realDir)) {
         goto next;
      }
      if (IsInUseStagingDir(apparentDir, realDir)) {
         goto next;
      }
      if (ChangedSince(realDir, cutoff, 0)) {
         goto next;
      }

      /* Only now, for a directory that is otherwise due, look at /proc. */
      if (!collected) {
         openPaths = CollectOpenPaths(&openCount);
         collected = TRUE;
      }
      if (openPaths == NULL) {
         goto next;
      }
      resolvedDir = realpath(realDir, NULL);
      for (j = 0; j < openCount && !open; j++) {
         open = PathIsUnder(openPaths[j], realDir) ||
                (resolvedDir != NULL && PathIsUnder(openPaths[j], resolvedDir));
      }
      if (open) {
         goto next;
      }

      MakeTreeWritable(realDir, 0);
      if (!File_DeleteDirectoryTree(realDir)) {
         Log("%s: could not remove %s\n", __FUNCTION__, realDir);
         goto next;
      }
      if (Posix_Lstat(apparentDir, &sb) == 0 && S_ISLNK(sb.st_mode) &&
          sb.st_uid == getuid() &&
          (link = Posix_ReadLink(apparentDir)) != NULL &&
          strcmp(link, realDir) == 0) {
         Posix_Unlink(apparentDir);
      }
      Log("%s: removed %s\n", __FUNCTION__, realDir);

next:
      free(link);
      free(resolvedDir);
      free(realDir);
      free(apparentDir);
   }
   Util_FreeStringList(names, count);
   if (openPaths != NULL) {
      Util_FreeStringList(openPaths, openCount);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnD_PruneStagingDirectories --
 *
 *      PruneStagingDirectories() on the user's staging roots, for a caller
 *      that is not about to create a staging directory, such as start-up.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See PruneStagingDirectories().
 *
 *-----------------------------------------------------------------------------
 */

void
DnD_PruneStagingDirectories(void)
{
   const char *realRoot = DetermineRealRootDirectory();
   const char *apparentRoot = DnD_GetFileRoot();

   if (realRoot != NULL && apparentRoot != NULL &&
       File_IsDirectory(realRoot) && File_IsDirectory(apparentRoot)) {
      PruneStagingDirectories(realRoot, apparentRoot);
   }
}


/* How often DnD_StartStagingPrune() checks again. */
#define DND_STAGING_PRUNE_INTERVAL_SEC 60

static guint gPruneTimer;


static gboolean
OnStagingPruneTimer(gpointer data)   // IN: unused
{
   DnD_PruneStagingDirectories();
   return G_SOURCE_CONTINUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnD_StartStagingPrune --
 *
 *      Prune the staging directories now, and again every
 *      DND_STAGING_PRUNE_INTERVAL_SEC from the main loop, so that a transfer
 *      that is over goes without waiting for the next one to start.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Adds a timer to the default main context.
 *
 *-----------------------------------------------------------------------------
 */

void
DnD_StartStagingPrune(void)
{
   DnD_PruneStagingDirectories();
   if (gPruneTimer == 0) {
      gPruneTimer = g_timeout_add_seconds(DND_STAGING_PRUNE_INTERVAL_SEC,
                                          OnStagingPruneTimer, NULL);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnD_StopStagingPrune --
 *
 *      Remove the timer DnD_StartStagingPrune() added.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
DnD_StopStagingPrune(void)
{
   if (gPruneTimer != 0) {
      g_source_remove(gPruneTimer);
      gPruneTimer = 0;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CreateStagingDirectory --
 *
 *      Creates a fresh staging directory within `realRoot` and creates a
 *      symlink to it within `apparentRoot`.
 *
 * Results:
 *      !NULL   Success.  Caller must free result.
 *      NULL    Failure.
 *
 * Side effects:
 *      Creates directories and stuff.
 *
 *-----------------------------------------------------------------------------
 */

static char *
CreateStagingDirectory(
   const char *realRoot,       // IN: e.g. $HOME/.cache/vmware/drag_and_drop/
   const char *apparentRoot)   // IN: e.g. /tmp/VMwareDnD/
{
   char *result = NULL;
   int i;

   for (i = 0; i < 10 && result == NULL; i++) {
      char *apparentStagingDir = NULL;
      // Reminder: mkdtemp updates its arg in-place.
      char *realStagingDir =
         Str_SafeAsprintf(NULL, "%s" DND_STAGING_NAME_TEMPLATE, realRoot);

      if (mkdtemp(realStagingDir) != NULL) {
         char *randomPart = strrchr(realStagingDir, '/') + 1;
         VERIFY(*randomPart != '\0');

         apparentStagingDir = Unicode_Append(apparentRoot, randomPart);

         if (Posix_Symlink(realStagingDir, apparentStagingDir) == 0) {
            // Transfer ownership to caller.
            result = apparentStagingDir;
            apparentStagingDir = NULL;
         } else {
            Warning("dnd: symlink(%s): %s", apparentStagingDir, Err_ErrString());
            Posix_Rmdir(realStagingDir);
         }
      } else {
         Warning("dnd: mkdtemp(%s): %s", realStagingDir, Err_ErrString());
      }

      free(realStagingDir);
      free(apparentStagingDir);
   }

   return result;
}
