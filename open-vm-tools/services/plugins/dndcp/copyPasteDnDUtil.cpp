/*********************************************************
 * Copyright (c) 2025 Broadcom. All Rights Reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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
 * copyPasteDnDUtil.cpp --
 *
 *    This is the utility file to keep the common API's
 *    for Copy/Paste and Drag/Drop functionality.
 *
 */


#include "copyPasteDnDUtil.h"
#include "file.h"
#include "str.h"
#include "dndFileContentsUtil.h"
#include "dndFileList.hh"
#include "copyPasteUIX11.h"
extern "C" {
#include "cpNameUtil.h"
}


/*
 *----------------------------------------------------------------------------
 *
 * LocalIsSafeStagingName --
 *
 *      Check that a host supplied file name is NUL terminated and can only
 *      name something below the staging directory it will be appended to.
 *
 *      Every path that joins a host supplied name onto a staging directory
 *      uses this check, so that they agree on what a staged name may be.
 *
 * Results:
 *      true if the name is safe to use, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

bool
LocalIsSafeStagingName(const char *name,     // IN: host supplied file name
                       size_t len)           // IN: bytes at name
{
   const char *comp = name;

   if (len == 0 || name[len - 1] != '\0') {
      return false;
   }

   if (*name == '\0' || File_IsFullPath(name)) {
      return false;
   }

   /*
    * CR and LF each end an entry in a text/uri-list, and this name is put
    * into one. A name carrying either would be published as several entries,
    * the extra ones naming whatever the host chose.
    */
   if (Str_Strchr(name, '\r') != NULL || Str_Strchr(name, '\n') != NULL) {
      return false;
   }

   while (comp != NULL) {
      if (Str_Strncmp(comp, "..", 2) == 0 &&
          (comp[2] == '\0' || comp[2] == DIRSEPC)) {
         return false;
      }
      comp = Str_Strchr(comp, DIRSEPC);
      if (comp != NULL) {
         comp++;
      }
   }

   return true;
}


/*
 *----------------------------------------------------------------------------
 *
 * LocalPrepareFileContents --
 *
 *      Try to extract file contents from Clipboard. Write all files to a
 *      temporary staging directory. Construct uri list.
 *
 * Results:
 *      true if success, false otherwise.
 *
 * Side effects:
 *      On success, records the staging directory as the one whose files the
 *      clipboard offers, so that the prune in dndXdg.c keeps it.
 *
 *----------------------------------------------------------------------------
 */

bool
LocalPrepareFileContents(const CPClipboard *clip,                       // IN
                         std::vector<utf::string>& fileContentsList)    // OUT
{
   void *buf = NULL;
   size_t sz = 0;
   XDR xdrs;
   CPFileContents fileContents;
   CPFileContentsList *contentsList = NULL;
   size_t nFiles = 0;
   CPFileItem *fileItem = NULL;
   char *tempDir = NULL;
   size_t i = 0;
   bool ret = false;

   if (!CPClipboard_GetItem(clip, CPFORMAT_FILECONTENTS, &buf, &sz)) {
      Warning("%s: CPClipboard_GetItem failed\n", __FUNCTION__);
      return false;
   }

   /* Extract file contents from buf. */
   xdrmem_create(&xdrs, (char *)buf, sz, XDR_DECODE);
   memset(&fileContents, 0, sizeof fileContents);

   if (!xdr_CPFileContents(&xdrs, &fileContents)) {
      Warning("%s: xdr_CPFileContents failed.\n", __FUNCTION__);
      xdr_destroy(&xdrs);
      return false;
   }
   xdr_destroy(&xdrs);

   contentsList = fileContents.CPFileContents_u.fileContentsV1;
   if (!contentsList) {
      Warning("%s: invalid contentsList.\n", __FUNCTION__);
      goto exit;
   }

   nFiles = contentsList->fileItem.fileItem_len;
   if (0 == nFiles) {
      Warning("%s: invalid nFiles.\n", __FUNCTION__);
      goto exit;
   }

   fileItem = contentsList->fileItem.fileItem_val;
   if (!fileItem) {
      Warning("%s: invalid fileItem.\n", __FUNCTION__);
      goto exit;
   }

   /*
    * Write files into a temporary staging directory. These files will be moved
    * to final destination, or deleted on next reboot.
    */
   tempDir = DnD_CreateStagingDirectory();
   if (!tempDir) {
      Warning("%s: DnD_CreateStagingDirectory failed.\n", __FUNCTION__);
      goto exit;
   }

   fileContentsList.clear();

   for (i = 0; i < nFiles; i++) {
      utf::string fileName;
      utf::string filePathName;
      VmTimeType createTime = -1;
      VmTimeType accessTime = -1;
      VmTimeType writeTime = -1;
      VmTimeType attrChangeTime = -1;

      if (!fileItem[i].cpName.cpName_val ||
          0 == fileItem[i].cpName.cpName_len) {
         Warning("%s: invalid fileItem[%" FMTSZ "u].cpName.\n", __FUNCTION__, i);
         goto exit;
      }

      /*
       * '\0' is used as directory separator in cross-platform name. Now turn
       * '\0' in data into DIRSEPC.
       *
       * Note that we don't convert the final '\0' into DIRSEPC so the string
       * is NUL terminated.
       */
      CPNameUtil_CharReplace(fileItem[i].cpName.cpName_val,
                             fileItem[i].cpName.cpName_len - 1,
                             '\0',
                             DIRSEPC);

      /*
       * The name is host supplied and is about to be joined onto the staging
       * directory, so it must not be able to name anything outside of it.
       */
      if (!LocalIsSafeStagingName(fileItem[i].cpName.cpName_val,
                                  fileItem[i].cpName.cpName_len)) {
         Warning("%s: unsafe fileItem[%" FMTSZ "u].cpName.\n", __FUNCTION__, i);
         goto exit;
      }

      fileName = fileItem[i].cpName.cpName_val;
      filePathName = tempDir;
      filePathName += DIRSEPS + fileName;

      if ((fileItem[i].validFlags & CP_FILE_VALID_TYPE) &&
          CP_FILE_TYPE_DIRECTORY == fileItem[i].type) {
         if (!File_CreateDirectory(filePathName.c_str())) {
            goto exit;
         }
         g_debug("%s: created directory [%s].\n",
                   __FUNCTION__, filePathName.c_str());
      } else if ((fileItem[i].validFlags & CP_FILE_VALID_TYPE) &&
                 CP_FILE_TYPE_REGULAR == fileItem[i].type) {
         FileIODescriptor file;
         FileIOResult fileErr;

         FileIO_Invalidate(&file);

         fileErr = FileIO_Open(&file,
                               filePathName.c_str(),
                               FILEIO_ACCESS_WRITE,
                               FILEIO_OPEN_CREATE_EMPTY);
         if (!FileIO_IsSuccess(fileErr)) {
            goto exit;
         }

         fileErr = FileIO_Write(&file,
                                fileItem[i].content.content_val,
                                fileItem[i].content.content_len,
                                NULL);

         FileIO_Close(&file);
         g_debug("%s: created file [%s].\n", __FUNCTION__, filePathName.c_str());
      } else {
         /*
          * Right now only Windows can provide CPFORMAT_FILECONTENTS data.
          * Symlink file is not expected. Continue with next file if the
          * type is not valid.
          */
         continue;
      }

      /* Update file time attributes. */
      createTime = (fileItem->validFlags & CP_FILE_VALID_CREATE_TIME) ?
         fileItem->createTime: -1;
      accessTime = (fileItem->validFlags & CP_FILE_VALID_ACCESS_TIME) ?
         fileItem->accessTime: -1;
      writeTime = (fileItem->validFlags & CP_FILE_VALID_WRITE_TIME) ?
         fileItem->writeTime: -1;
      attrChangeTime = (fileItem->validFlags & CP_FILE_VALID_CHANGE_TIME) ?
         fileItem->attrChangeTime: -1;

      if (!File_SetTimes(filePathName.c_str(),
                         createTime,
                         accessTime,
                         writeTime,
                         attrChangeTime)) {
         /* Not a critical error, only log it. */
         g_debug("%s: File_SetTimes failed with file [%s].\n",
               __FUNCTION__, filePathName.c_str());
      }

      /* Update file permission attributes. */
      if (fileItem->validFlags & CP_FILE_VALID_PERMS) {
         if (Posix_Chmod(filePathName.c_str(),
                         fileItem->permissions) < 0) {
            /* Not a critical error, only log it. */
            g_debug("%s: Posix_Chmod failed with file [%s].\n",
                  __FUNCTION__, filePathName.c_str());
         }
      }

      /*
       * If there is no DIRSEPC inside the fileName, this file/directory is a
       * top level one. We only put top level name into uri list.
       */
      if (fileName.find(DIRSEPS, 0) == utf::string::npos) {
         fileContentsList.push_back(filePathName);
      }
   }
   g_debug("%s: created uri list\n", __FUNCTION__);
#ifdef DND_IS_XDG
   DnD_SetClipboardStagingDir(tempDir);
#endif
   ret = true;

exit:
   xdr_free((xdrproc_t)xdr_CPFileContents, (char *)&fileContents);
   if (tempDir && !ret) {
      DnD_DeleteStagingFiles(tempDir, FALSE);
   }
   free(tempDir);
   return ret;
}
