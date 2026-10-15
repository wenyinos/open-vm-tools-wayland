/*********************************************************
 * Copyright (C) 2026 Broadcom. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Lesser GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/**
 * @file rtfToText.c
 *
 *    Render rich text as plain text.
 *
 *    RTF is a stream of groups between braces. A group opens with a control
 *    word that says what it is for; some of them describe the document rather
 *    than its text, such as the font table, and their contents must not reach
 *    the output. Everything else is text, with control words for layout and
 *    escapes for characters that the syntax would otherwise eat.
 *
 *    The renderer keeps a stack of groups so that it knows when it is inside a
 *    destination to drop, and a buffer for the bytes it has seen, so that text
 *    split by control words comes out as it reads.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define G_LOG_DOMAIN "dndcp"

#include "rtfToText.h"

#include <string.h>

/* How deep groups may nest before the input is taken for malformed. */
#define RTF_MAX_DEPTH 256

/*
 * Destinations whose contents are not document text. The font and colour
 * tables, the stylesheet and the document properties all carry words that
 * would otherwise read as text.
 */
static const char * const gSkipDestinations[] = {
   "fonttbl", "colortbl", "stylesheet", "info", "pict", "object",
   "header", "footer", "headerl", "headerr", "footerl", "footerr",
   "footnote", "annotation", "field", "fldinst", "listtable",
   "listoverridetable", "rsidtbl", "generator", "filetbl", "revtbl",
   "themedata", "colorschememapping", "latentstyles", "datastore",
   "xmlnstbl", "mmathPr", "docvar", "panose", "falt", "fname",
   NULL,
};


/*
 *-----------------------------------------------------------------------------
 *
 * RtfIsSkippedDestination --
 *
 *    Whether a control word names a destination whose contents are not text.
 *
 * Results:
 *    TRUE to drop the group the word opened.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
RtfIsSkippedDestination(const char *word)  // IN: control word, no backslash
{
   int i;

   for (i = 0; gSkipDestinations[i] != NULL; i++) {
      if (strcmp(word, gSkipDestinations[i]) == 0) {
         return TRUE;
      }
   }

   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * RtfParseUnicodeEscape --
 *
 *    Parse a \uNNNN escape at `i` and the substitute character after it.
 *
 *    The value is a signed 16-bit number, so a character above the basic
 *    multilingual plane arrives as two of these: the high surrogate first,
 *    then the low one. Each is followed by a substitute character for readers
 *    without Unicode support, which is dropped either way.
 *
 * Results:
 *    TRUE when an escape was read. *cp receives the value as an unsigned code
 *    unit and *next the offset just past the escape and its substitute.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
RtfParseUnicodeEscape(const char *rtf,  // IN: document
                      size_t len,       // IN: its length
                      size_t i,         // IN: offset of the backslash
                      guint32 *cp,      // OUT: code unit
                      size_t *next)     // OUT: offset past the escape
{
   size_t j = i + 1;
   gboolean negative = FALSE;
   long param = 0;
   gboolean haveParam = FALSE;

   if (j >= len || rtf[j] != 'u') {
      return FALSE;
   }
   j++;

   if (j < len && rtf[j] == '-') {
      negative = TRUE;
      j++;
   }
   while (j < len && g_ascii_isdigit(rtf[j])) {
      param = param * 10 + (rtf[j] - '0');
      haveParam = TRUE;
      j++;
   }
   if (!haveParam) {
      return FALSE;
   }
   if (negative) {
      param = -param;
   }
   /* One space after the digits is the delimiter and not text. */
   if (j < len && rtf[j] == ' ') {
      j++;
   }
   /* The substitute character, if present. */
   if (j < len && rtf[j] == '?') {
      j++;
   }

   *cp = (guint32) (param < 0 ? param + 65536 : param);
   *next = j;

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * RtfToText --
 *
 *    Render an RTF document as plain UTF-8 text.
 *
 *    The input is walked once. Group depth is tracked so that a destination
 *    can be dropped whole, including anything it nests, and layout words are
 *    turned into the characters they stand for: a paragraph ends with a
 *    newline, a tab is a tab.
 *
 *    Escapes are resolved: \\ \{ \} stand for the character itself, \'hh is a
 *    byte in the current code page and \uNNNN a Unicode code unit followed by
 *    a substitute character that is dropped.
 *
 * Results:
 *    A newly allocated string, or NULL when the input is not RTF or declares
 *    no text. The caller frees it with g_free().
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

gchar *
RtfToText(const char *rtf,     // IN: document
          size_t len,          // IN: its length
          size_t *outLen)      // OUT: length of the result, without NUL
{
   GString *out;
   size_t i = 0;
   int depth = 0;
   int skipDepth = 0;           /* Group depth at which to stop dropping. */

   if (outLen != NULL) {
      *outLen = 0;
   }
   if (rtf == NULL || len < 6 || memcmp(rtf, "{\\rtf", 5) != 0) {
      return NULL;
   }

   out = g_string_sized_new(MIN(len, 4096) + 1);

   while (i < len) {
      char c = rtf[i];

      if (c == '{') {
         depth++;
         if (depth > RTF_MAX_DEPTH) {
            g_debug("%s: groups nested too deeply, stopping\n", __FUNCTION__);
            break;
         }
         i++;

         /*
          * A group that opens with a control word either describes the
          * document, in which case its whole contents are dropped, or says
          * how the text is laid out, in which case only the word matters.
          */
         if (i < len && rtf[i] == '\\') {
            size_t j = i + 1;
            gboolean ignorable = FALSE;

            if (j < len && rtf[j] == '*') {
               /* \* marks a destination a reader may skip. */
               ignorable = TRUE;
               j++;
            }
            {
               size_t wordStart = j;
               char word[32];
               size_t wordLen;

               while (j < len && g_ascii_isalpha(rtf[j])) {
                  j++;
               }
               wordLen = j - wordStart;
               if (wordLen > 0 && wordLen < sizeof word) {
                  memcpy(word, rtf + wordStart, wordLen);
                  word[wordLen] = '\0';
                  if (ignorable || RtfIsSkippedDestination(word)) {
                     skipDepth = depth;
                  }
               } else if (ignorable) {
                  skipDepth = depth;
               }
            }
         }
         continue;
      }

      if (c == '}') {
         if (skipDepth != 0 && depth == skipDepth) {
            skipDepth = 0;
         }
         depth--;
         i++;
         continue;
      }

      if (skipDepth != 0) {
         i++;
         continue;
      }

      if (c == '\\') {
         size_t j = i + 1;

         if (j >= len) {
            break;
         }

         if (rtf[j] == '\\' || rtf[j] == '{' || rtf[j] == '}') {
            g_string_append_c(out, rtf[j]);
            i = j + 1;
            continue;
         }

         if (rtf[j] == '\'') {
            /* \'hh is one byte, which for the host's clips is Latin-1
             * text. The byte is emitted as UTF-8 so the result is valid. */
            unsigned int byte = 0;
            int k;

            for (k = 0; k < 2 && j + 1 + k < len; k++) {
               char h = rtf[j + 1 + k];

               if (!g_ascii_isxdigit(h)) {
                  break;
               }
               byte = byte * 16 + (g_ascii_isdigit(h) ? h - '0'
                                                      : (g_ascii_tolower(h) - 'a' + 10));
            }
            if (k == 2) {
               g_string_append_unichar(out, (gunichar) byte);
               i = j + 3;
               continue;
            }
            i = j + 1;
            continue;
         }

         if (rtf[j] == 'u') {
            guint32 cp = 0;
            size_t next = i;

            if (RtfParseUnicodeEscape(rtf, len, i, &cp, &next)) {
               /*
                * A character above the basic multilingual plane arrives as a
                * surrogate pair, high half first. Combine the two, and drop a
                * half that stands alone, since UTF-8 has no encoding for one.
                */
               if (cp >= 0xd800 && cp <= 0xdbff) {
                  guint32 low = 0;
                  size_t afterLow = next;

                  if (next < len && rtf[next] == '\\' &&
                      RtfParseUnicodeEscape(rtf, len, next, &low, &afterLow) &&
                      low >= 0xdc00 && low <= 0xdfff) {
                     cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                     next = afterLow;
                  } else {
                     g_debug("%s: lone surrogate dropped\n", __FUNCTION__);
                     i = next;
                     continue;
                  }
               } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                  /* A low half with no high half before it. */
                  g_debug("%s: lone surrogate dropped\n", __FUNCTION__);
                  i = next;
                  continue;
               }
               g_string_append_unichar(out, (gunichar) cp);
               i = next;
               continue;
            }
         }

         {
            size_t wordStart = j;
            char word[32];
            size_t wordLen;

            while (j < len && g_ascii_isalpha(rtf[j])) {
               j++;
            }
            wordLen = j - wordStart;

            if (wordLen == 0) {
               /* A control symbol, not a word. Nothing here renders it. */
               i = j + (j < len ? 1 : 0);
               continue;
            }

            while (j < len && (g_ascii_isdigit(rtf[j]) || rtf[j] == '-')) {
               j++;
            }
            /* One space after a word is the delimiter and not text. */
            if (j < len && rtf[j] == ' ') {
               j++;
            }
            i = j;

            if (wordLen >= sizeof word) {
               continue;
            }
            memcpy(word, rtf + wordStart, wordLen);
            word[wordLen] = '\0';

            if (strcmp(word, "par") == 0 || strcmp(word, "line") == 0 ||
                strcmp(word, "sect") == 0 || strcmp(word, "page") == 0) {
               g_string_append_c(out, '\n');
            } else if (strcmp(word, "tab") == 0) {
               g_string_append_c(out, '\t');
            }
            /*
             * Anything else is a layout word, such as \b or \fs24, which has
             * no plain text form here.
             */
            continue;
         }
      }

      if (c == '\r' || c == '\n') {
         /* Line breaks in the source are not breaks in the text. */
         i++;
         continue;
      }

      /*
       * Text. RTF is a byte stream in the document's code page, which the
       * host's clips use for ASCII; anything above it arrives through the
       * escapes above.
       */
      g_string_append_c(out, c);
      i++;
   }

   if (out->len == 0) {
      g_string_free(out, TRUE);
      return NULL;
   }

   if (outLen != NULL) {
      *outLen = out->len;
   }

   return g_string_free(out, FALSE);
}
