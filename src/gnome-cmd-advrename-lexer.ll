/*
    GNOME Commander - A GNOME based file manager
    Copyright (C) 2001-2006 Marcus Bjurman
    Copyright (C) 2007-2008 Piotr Eljasiak

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
*/


%option noyywrap
%option nounput


%{
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>


#include "gnome-cmd-includes.h"
#include "gnome-cmd-file.h"
#include "gnome-cmd-advrename-lexer.h"
#include "tags/gnome-cmd-tags.h"
#include "utils.h"

using namespace std;


#define   ECHO  {                                                                     \
                  CHUNK *p = g_new0 (CHUNK, 1);                                       \
                                                                                      \
                  p->type = TEXT;                                                     \
                  p->s = g_string_new(yytext);                                        \
                  fname_template.push_back(p);                                        \
                }


#define   MAX_PRECISION           16
#define   MAX_XRANDOM_PRECISION    8u

enum {TEXT=1,NAME,EXTENSION,FULL_NAME,COUNTER,XRANDOM,XXRANDOM,PARENT_DIR,GRANDPARENT_DIR,METATAG};

typedef struct
{
  int type;
  union
  {
    GString *s;
    struct
    {
      int beg;          // default: 0
      int end;          // default: 0
      char *name;       // default: NULL
      GnomeCmdTag tag;  // default: TAG_NONE
      GList *opt;       // default: NULL
    } tag;
    struct
    {
      unsigned long n;  // default: start
      int start;        // default: default_counter_start (1)
      int step;         // default: default_counter_step  (1)
      int prec;         // default: default_counter_prec (-1)
    } counter;
    struct
    {
      guint x_prec;     // default: MAX_XRANDOM_PRECISION  (8)
    } random;
  };
} CHUNK;


static vector<CHUNK *> fname_template;


static unsigned long default_counter_start = 1;
static unsigned      default_counter_step = 1;
static unsigned      default_counter_prec = -1;
static char          counter_fmt[8] = "%lu";

%}

int        -?[0-9]+
uint       0*[1-9][0-9]*

range      {int}|{int}?:{int}?|{int},{uint}

ape         [aA][pP][eE]
audio       [aA][uU][dD][iI][oO]
chm         [cC][hH][mM]
doc         [dD][oO][cC]
exif        [eE][xX][iI][fF]
file        [fF][iI][lL][eE]
flac        [fF][lL][aA][cC]
icc         [iI][cC][cC]
id3         [iI][dD]3
image       [iI][mM][aA][gG][eE]
iptc        [iI][pP][tT][cC]
pdf         [pP][dD][fF]
rpm         [rR][pP][mM]
vorbis      [vV][oO][rR][bB][iI][sS]

tag_name    {ape}|{audio}|{doc}|{exif}|{file}|{flac}|{id3}|{image}|{iptc}|{pdf}|{vorbis}

%%


%{
  static int from, to;
%}


\$[egnNp]\({range}\)            {
                                  gchar **a = g_strsplit_set(yytext+3,":,()",0);

                                  CHUNK *p = g_new0 (CHUNK,1);

                                  switch (yytext[1])
                                  {
                                    case 'e' : p->type = EXTENSION;       break;
                                    case 'g' : p->type = GRANDPARENT_DIR; break;
                                    case 'n' : p->type = NAME;            break;
                                    case 'N' : p->type = FULL_NAME;       break;
                                    case 'p' : p->type = PARENT_DIR;      break;
                                  }

                                  from = to = 0;

                                  switch (g_strv_length(a))                 // glib >= 2.6
                                  {
                                      case 2:
                                          sscanf(a[0],"%d",&from);
                                          break;
                                      case 3:
                                          sscanf(a[0],"%d",&from);
                                          sscanf(a[1],"%d",&to);
                                          if (strchr(yytext+3,','))
                                              to = from<0 && to+from>0 ? 0 : from+to;
                                          break;
                                  }

                                  g_strfreev(a);

                                  p->tag.beg = from;
                                  p->tag.end = to;
                                  p->tag.name = NULL;
                                  p->tag.opt = NULL;

                                  fname_template.push_back(p);
                                }

\$[c]\({uint}\)                 {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  int precision = default_counter_prec;

                                  sscanf(yytext+3,"%d",&precision);

                                  p->type = COUNTER;
                                  p->counter.n = p->counter.start = default_counter_start;
                                  p->counter.step = default_counter_step;
                                  p->counter.prec = min (precision, MAX_PRECISION);

                                  fname_template.push_back(p);
                                }

\$[xX]\({uint}\)                {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  guint precision = MAX_XRANDOM_PRECISION;

                                  sscanf(yytext+3,"%u",&precision);

                                  switch (yytext[1])
                                  {
                                    case 'x' : p->type = XRANDOM;       break;
                                    case 'X' : p->type = XXRANDOM;      break;
                                  }
                                  p->random.x_prec = min (precision, MAX_XRANDOM_PRECISION);

                                  fname_template.push_back(p);
                                }

\$T\({tag_name}(\.[a-zA-Z][a-zA-Z0-9]+)+(,[^,)]+)*\)   {

                                  gchar **a = g_strsplit_set(yytext+3,",()",0);
                                  guint n = g_strv_length(a);                     // glib >= 2.6

                                  CHUNK *p = g_new0 (CHUNK,1);

                                  int i;

                                  p->type = METATAG;
                                  p->tag.name = g_strdup (a[0]);
                                  p->tag.tag = gcmd_tags_get_tag_by_name(a[0]);
                                  p->tag.opt = NULL;

                                  for (i=n-2; i>0; --i)
                                    p->tag.opt = g_list_prepend(p->tag.opt, (gpointer) g_string_new(a[i]));

                                  g_strfreev(a);

                                  fname_template.push_back(p);
                                }

\$[cxXegnNp]\([^\)]*\)?         ECHO;                                      // don't substitute broken $x tokens like $x(-1), $x(abc) or $x(abc

\$[egnNp]                       {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  switch (yytext[1])
                                  {
                                    case 'e' : p->type = EXTENSION;       break;
                                    case 'g' : p->type = GRANDPARENT_DIR; break;
                                    case 'n' : p->type = NAME;            break;
                                    case 'N' : p->type = FULL_NAME;       break;
                                    case 'p' : p->type = PARENT_DIR;      break;
                                  }

                                  p->tag.beg = 0;
                                  p->tag.end = 0;
                                  p->tag.name = NULL;
                                  p->tag.opt = NULL;

                                  fname_template.push_back(p);
                                }

\$[c]                           {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  p->type = COUNTER;
                                  p->counter.n = p->counter.start = default_counter_start;
                                  p->counter.step = default_counter_step;
                                  p->counter.prec = default_counter_prec;

                                  fname_template.push_back(p);
                                }

\$[xX]                          {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  switch (yytext[1])
                                  {
                                    case 'x' : p->type = XRANDOM;       break;
                                    case 'X' : p->type = XXRANDOM;      break;
                                  }
                                  p->random.x_prec = MAX_XRANDOM_PRECISION;

                                  fname_template.push_back(p);
                                }

\$\$                            {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  p->type = TEXT;
                                  p->s = g_string_new("$");

                                  fname_template.push_back(p);
                                }

%[Dnt]                          {
                                  CHUNK *p = g_new0 (CHUNK,1);

                                  p->type = TEXT;
                                  p->s = g_string_new("%%");

                                  fname_template.push_back(p);
                                }
%%


//  TODO:  since counters are to be indivual, it's necessary to provide mechanism for resetting/changing implicit parameters - $c

void gnome_cmd_advrename_reset_counter(unsigned start, unsigned precision, unsigned step)
{
  for (vector<CHUNK *>::iterator i=fname_template.begin(); i!=fname_template.end(); ++i)
    if ((*i)->type==COUNTER)
      (*i)->counter.n = (*i)->counter.start;

  default_counter_start = start;
  default_counter_step = step;
  default_counter_prec = precision;
  sprintf(counter_fmt,"%%0%ulu",(precision<MAX_PRECISION ? precision : MAX_PRECISION));
}


void gnome_cmd_advrename_parse_template(const char *template_string)
{
  for (vector<CHUNK *>::iterator i=fname_template.begin(); i!=fname_template.end(); ++i)
    switch ((*i)->type)
    {
      case TEXT:
          g_string_free((*i)->s,TRUE);
          g_free(*i);
          break;

      case METATAG:
          // FIXME: free memory here for all (*i) members
          g_free((*i)->tag.name);
          g_free(*i);
          break;
    }

  fname_template.clear();

  yy_scan_string(template_string);
  yylex();
  yy_delete_buffer(YY_CURRENT_BUFFER);
}


// gboolean is_substr (const CHUNK *p)
// {
  // return p->tag.beg!=0 || p->tag.end!=0;
// }


inline void mk_substr (int src_len, const CHUNK *p, int &pos, int &len)
{
  pos = p->tag.beg<0 ? p->tag.beg+src_len : p->tag.beg;
  pos = max(pos, 0);

  if (pos>=src_len)
  {
    pos = len = 0;
    return;
  }

  len = p->tag.end>0 ? p->tag.end-pos : src_len+p->tag.end-pos;
  len = CLAMP (len, 0, src_len-pos);
}


inline void append_utf8_chunk (string &s, const CHUNK *p, const char *path, int path_len)
{
  // if (!is_substr (p))
  // {
    // s += path_offset ? g_utf8_offset_to_pointer (path, path_offset) : path;
    // return;
  // }

  int from, length;

  mk_substr (path_len, p, from, length);

  if (!length)
    return;

  const char *beg = g_utf8_offset_to_pointer (path, from);
  const char *end = g_utf8_offset_to_pointer (beg, length);

  s.append(path, beg-path, end-beg);
}


inline void find_dirs (const gchar *path, const gchar *&parent_dir, const gchar *&grandparent_dir, int &parent_dir_len, int &grandparent_dir_len)
{
    const gchar *dir0 = "";

    grandparent_dir = parent_dir = dir0;

    int offset = 0;

    int offset0 = 0;
    int offset1 = 0;
    int offset2 = 0;

    for (const gchar *s = path; *s;)
    {
        gboolean sep = *s==G_DIR_SEPARATOR;

        s = g_utf8_next_char (s);
        ++offset;

        if (!sep)
            continue;

        grandparent_dir = parent_dir;
        parent_dir = dir0;
        dir0 = s;

        offset2 = offset1;
        offset1 = offset0;
        offset0 = offset;
    }

    parent_dir_len = max(offset0-offset1-1,0);
    grandparent_dir_len = max(offset1-offset2-1,0);
}


char *gnome_cmd_advrename_gen_fname (char *new_fname, size_t new_fname_size, GnomeCmdFile *finfo)
{
  string fmt;
  fmt.reserve(256);

  char *fname = get_utf8 (finfo->info->name);
  char *ext = g_utf8_strrchr (fname, -1, '.');

  int full_name_len = g_utf8_strlen (fname, -1);
  int name_len = full_name_len;
  int ext_len = 0;

  if (!ext)  ext = "";  else
  {
    ++ext;
    ext_len = g_utf8_strlen(ext, -1);
    name_len = full_name_len - ext_len - 1;
  }

  const char *parent_dir, *grandparent_dir;
  int parent_dir_len, grandparent_dir_len;

  find_dirs(gnome_cmd_file_get_path(finfo), parent_dir, grandparent_dir, parent_dir_len, grandparent_dir_len);

  for (vector<CHUNK *>::iterator i=fname_template.begin(); i!=fname_template.end(); ++i)
    switch ((*i)->type)
    {
      case TEXT  :
                    fmt += (*i)->s->str;
                    break;

      case NAME  :
                    append_utf8_chunk (fmt, *i, fname, name_len);
                    break;

      case EXTENSION:
                    append_utf8_chunk (fmt, *i, ext, ext_len);
                    break;

      case FULL_NAME:
                    append_utf8_chunk (fmt, *i, fname, full_name_len);
                    break;

      case PARENT_DIR:
                    append_utf8_chunk (fmt, *i, parent_dir, parent_dir_len);
                    break;

      case GRANDPARENT_DIR:
                    append_utf8_chunk (fmt, *i, grandparent_dir, grandparent_dir_len);
                    break;

      case COUNTER:
                    {
                      static char custom_counter_fmt[8];
                      static char counter_value[MAX_PRECISION+1];

                      if ((*i)->counter.prec!=-1)
                        sprintf(custom_counter_fmt,"%%0%ilu",(*i)->counter.prec);

                      snprintf (counter_value, MAX_PRECISION, ((*i)->counter.prec==-1 ? counter_fmt : custom_counter_fmt), (*i)->counter.n);
                      fmt += counter_value;

                      (*i)->counter.n += (*i)->counter.step;
                    }
                    break;

      case XRANDOM:
      case XXRANDOM:
                    {
                      static char custom_counter_fmt[8];
                      static char random_value[MAX_XRANDOM_PRECISION+1];

                      sprintf (custom_counter_fmt, "%%0%u%c", (*i)->random.x_prec, (*i)->type==XRANDOM ? 'x' : 'X');
                      snprintf (random_value, MAX_XRANDOM_PRECISION+1, custom_counter_fmt, (*i)->random.x_prec<MAX_XRANDOM_PRECISION ? g_random_int_range (0,1 << 4*(*i)->random.x_prec)
                                                                                                                                     : g_random_int ());
                      fmt += random_value;
                    }
                    break;

      case METATAG: // currently ranges are NOT supported for $T() tokens !!!

                    // const gchar *tag_value = gcmd_tags_get_value (finfo,(*i)->tag.tag);

                    // if (tag_value)
                      // append_utf8_chunk (fmt, *i, tag_value, g_utf8_strlen (tag_value, -1));

                    fmt += gcmd_tags_get_value (finfo,(*i)->tag.tag);
                    break;

      default :     break;
    }

  strftime(new_fname, new_fname_size, fmt.c_str(), localtime(&finfo->info->mtime));

  g_free(fname);

  return new_fname;
}
