/* Stabs in sections linking support.
   Copyright (C) 1996-2023 Free Software Foundation, Inc.
   Written by Ian Lance Taylor, Cygnus Support.

   This file is part of BFD, the Binary File Descriptor library.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */


/* This file contains support for linking stabs in sections, as used
   on COFF and ELF.  */

#include "sysdep.h"
#include "bfd.h"
#include "libbfd.h"
#include "aout/stab_gnu.h"
#include "safe-ctype.h"

/* Stabs entries use a 12 byte format:
     4 byte string table index
     1 byte stab type
     1 byte stab other field
     2 byte stab desc field
     4 byte stab value
   FIXME: This will have to change for a 64 bit object format.

   The stabs symbols are divided into compilation units.  For the
   first entry in each unit, the type of 0, the value is the length of
   the string table for this unit, and the desc field is the number of
   stabs symbols for this unit.  */

#define STRDXOFF  0
#define TYPEOFF   4
#define OTHEROFF  5
#define DESCOFF   6
#define VALOFF    8
#define STABSIZE  12

/* A linked list of totals that we have found for a particular header
   file.  A total is a unique identifier for a particular BINCL...EINCL
   sequence of STABs that can be used to identify duplicate sequences.
   It consists of three fields, 'sum_chars' which is the sum of all the
   STABS characters; 'num_chars' which is the number of these charactes
   and 'symb' which is a buffer of all the symbols in the sequence.  This
   buffer is only checked as a last resort.  */

struct stab_link_includes_totals
{
  struct stab_link_includes_totals *next;
  bfd_vma sum_chars;  /* Accumulated sum of STABS characters.  */
  bfd_vma num_chars;  /* Number of STABS characters.  */
  const char* symb;   /* The STABS characters themselves.  */
};

/* An entry in the header file hash table.  */

struct stab_link_includes_entry
{
  struct bfd_hash_entry root;
  /* List of totals we have found for this file.  */
  struct stab_link_includes_totals *totals;
};

/* This structure is used to hold a list of N_BINCL symbols, some of
   which might be converted into N_EXCL symbols.  */

struct stab_excl_list
{
  /* The next symbol to convert.  */
  struct stab_excl_list *next;
  /* The offset to this symbol in the section contents.  */
  bfd_size_type offset;
  /* The value to use for the symbol.  */
  bfd_vma val;
  /* The type of this symbol (N_BINCL or N_EXCL).  */
  int type;
};

/* This structure is stored with each .stab section.  */

struct stab_section_info
{
  /* This is a linked list of N_BINCL symbols which should be
     converted into N_EXCL symbols.  */
  struct stab_excl_list *excls;

  /* This is used to map input stab offsets within their sections
     to output stab offsets, to take into account stabs that have
     been deleted.  If it is NULL, the output offsets are the same
     as the input offsets, because no stabs have been deleted from
     this section.  Otherwise the i'th entry is the number of
     bytes of stabs that have been deleted prior to the i'th
     stab.  */
  bfd_size_type *cumulative_skips;

  /* This is an array of string indices.  For each stab symbol, we
     store the string index here.  If a stab symbol should not be
     included in the final output, the string index is -1.  */
  bfd_size_type stridxs[1];
};

/*
EXTERNAL
.{* This structure is used to keep track of stabs in sections
.   information while linking.  *}
.
.struct stab_info
.{
.  {* A hash table used to hold stabs strings.  *}
.  struct bfd_strtab_hash *strings;
.  {* The header file hash table.  *}
.  struct bfd_hash_table includes;
.  {* The first .stabstr section.  *}
.  struct bfd_section *stabstr;
.};
.
*/

/* The function to create a new entry in the header file hash table.  */

static struct bfd_hash_entry *
stab_link_includes_newfunc (struct bfd_hash_entry *entry,
			    struct bfd_hash_table *table,
			    const char *string)
{
  struct stab_link_includes_entry *ret =
    (struct stab_link_includes_entry *) entry;

  /* Allocate the structure if it has not already been allocated by a
     subclass.  */
  if (ret == NULL)
    ret = (struct stab_link_includes_entry *)
	bfd_hash_allocate (table, sizeof (struct stab_link_includes_entry));
  if (ret == NULL)
    return NULL;

  /* Call the allocation method of the superclass.  */
  ret = ((struct stab_link_includes_entry *)
	 bfd_hash_newfunc ((struct bfd_hash_entry *) ret, table, string));
  if (ret)
    /* Set local fields.  */
    ret->totals = NULL;

  return (struct bfd_hash_entry *) ret;
}

/*
INTERNAL_FUNCTION
	_bfd_link_section_stabs

SYNOPSIS
	bool _bfd_link_section_stabs
	  (bfd *, struct stab_info *, asection *, asection *, void **,
	   bfd_size_type *);

DESCRIPTION
	This function is called for each input file from the add_symbols
	pass of the linker.
*/

bool
_bfd_link_section_stabs (bfd *abfd,
			 struct stab_info *sinfo,
			 asection *stabsec,
			 asection *stabstrsec,
			 void * *psecinfo,
			 bfd_size_type *pstring_offset)
{
  bool first;
  bfd_size_type count, amt;
  struct stab_section_info *secinfo;
  bfd_byte *stabbuf = NULL;
  bfd_byte *stabstrbuf = NULL;
  bfd_byte *sym, *symend;
  bfd_size_type stroff, next_stroff, skip;
  bfd_size_type *pstridx;

  if (stabsec->size == 0
      || stabstrsec->size == 0
      || (stabsec->flags & SEC_HAS_CONTENTS) == 0
      || (stabstrsec->flags & SEC_HAS_CONTENTS) == 0)
    /* This file does not contain stabs debugging information.  */
    return true;

  if (stabsec->size % STABSIZE != 0)
    /* Something is wrong with the format of these stab symbols.
       Don't try to optimize them.  */
    return true;

  if ((stabstrsec->flags & SEC_RELOC) != 0)
    /* We shouldn't see relocations in the strings, and we aren't
       prepared to handle them.  */
    return true;

  if (bfd_is_abs_section (stabsec->output_section)
      || bfd_is_abs_section (stabstrsec->output_section))
    /* At least one of the sections is being discarded from the
       link, so we should just ignore them.  */
    return true;

  first = false;

  if (sinfo->stabstr == NULL)
    {
      flagword flags;

      /* Initialize the stabs information we need to keep track of.  */
      first = true;
      sinfo->strings = _bfd_stringtab_init ();
      if (sinfo->strings == NULL)
	goto error_return;
      /* Make sure the first byte is zero.  */
      (void) _bfd_stringtab_add (sinfo->strings, "", true, true);
      if (! bfd_hash_table_init (&sinfo->includes,
				 stab_link_includes_newfunc,
				 sizeof (struct stab_link_includes_entry)))
	goto error_return;
      flags = (SEC_HAS_CONTENTS | SEC_READONLY | SEC_DEBUGGING
	       | SEC_LINKER_CREATED);
      sinfo->stabstr = bfd_make_section_anyway_with_flags (abfd, ".stabstr",
							   flags);
      if (sinfo->stabstr == NULL)
	goto error_return;
    }

  /* Initialize the information we are going to store for this .stab
     section.  */
  count = stabsec->size / STABSIZE;

  amt = sizeof (struct stab_section_info);
  amt += (count - 1) * sizeof (bfd_size_type);
  *psecinfo = bfd_alloc (abfd, amt);
  if (*psecinfo == NULL)
    goto error_return;

  secinfo = (struct stab_section_info *) *psecinfo;
  secinfo->excls = NULL;
  stabsec->rawsize = stabsec->size;
  secinfo->cumulative_skips = NULL;
  memset (secinfo->stridxs, 0, (size_t) count * sizeof (bfd_size_type));

  /* Read the stabs information from abfd.  */
  if (!bfd_malloc_and_get_section (abfd, stabsec, &stabbuf)
      || !bfd_malloc_and_get_section (abfd, stabstrsec, &stabstrbuf))
    goto error_return;

  /* Look through the stabs symbols, work out the new string indices,
     and identify N_BINCL symbols which can be eliminated.  */
  stroff = 0;
  /* The stabs sections can be split when
     -split-by-reloc/-split-by-file is used.  We must keep track of
     each stab section's place in the single concatenated string
     table.  */
  next_stroff = pstring_offset ? *pstring_offset : 0;
  skip = 0;

  symend = stabbuf + stabsec->size;
  for (sym = stabbuf, pstridx = secinfo->stridxs;
       sym < symend;
       sym += STABSIZE, ++pstridx)
    {
      bfd_size_type symstroff;
      int type;
      const char *string;

      if (*pstridx != 0)
	/* This symbol has already been handled by an N_BINCL pass.  */
	continue;

      type = sym[TYPEOFF];

      if (type == 0)
	{
	  /* Special type 0 stabs indicate the offset to the next
	     string table.  We only copy the very first one.  */
	  stroff = next_stroff;
	  next_stroff += bfd_get_32 (abfd, sym + 8);
	  if (pstring_offset)
	    *pstring_offset = next_stroff;
	  if (! first)
	    {
	      *pstridx = (bfd_size_type) -1;
	      ++skip;
	      continue;
	    }
	  first = false;
	}

      /* Store the string in the hash table, and record the index.  */
      symstroff = stroff + bfd_get_32 (abfd, sym + STRDXOFF);
      if (symstroff >= stabstrsec->size)
	{
	  _bfd_error_handler
	    /* xgettext:c-format */
	    (_("%pB(%pA+%#lx): stabs entry has invalid string index"),
	     abfd, stabsec, (long long) (sym - stabbuf));
	  bfd_set_error (bfd_error_bad_value);
	  goto error_return;
	}
      string = (char *) stabstrbuf + symstroff;
      *pstridx = _bfd_stringtab_add (sinfo->strings, string, true, true);

      /* An N_BINCL symbol indicates the start of the stabs entries
	 for a header file.  We need to scan ahead to the next N_EINCL
	 symbol, ignoring nesting, adding up all the characters in the
	 symbol names, not including the file numbers in types (the
	 first number after an open parenthesis).  */
      if (type == (int) N_BINCL)
	{
	  bfd_vma sum_chars;
	  bfd_vma num_chars;
	  bfd_vma buf_len = 0;
	  char * symb;
	  char * symb_rover;
	  int nest;
	  bfd_byte * incl_sym;
	  struct stab_link_includes_entry * incl_entry;
	  struct stab_link_includes_totals * t;
	  struct stab_excl_list * ne;

	  symb = symb_rover = NULL;
	  sum_chars = num_chars = 0;
	  nest = 0;

	  for (incl_sym = sym + STABSIZE;
	       incl_sym < symend;
	       incl_sym += STABSIZE)
	    {
	      int incl_type;

	      incl_type = incl_sym[TYPEOFF];
	      if (incl_type == 0)
		break;
	      else if (incl_type == (int) N_EXCL)
		continue;
	      else if (incl_type == (int) N_EINCL)
		{
		  if (nest == 0)
		    break;
		  --nest;
		}
	      else if (incl_type == (int) N_BINCL)
		++nest;
	      else if (nest == 0)
		{
		  const char *str;

		  str = ((char *) stabstrbuf
			 + stroff
			 + bfd_get_32 (abfd, incl_sym + STRDXOFF));
		  for (; *str != '\0'; str++)
		    {
		      if (num_chars >= buf_len)
			{
			  buf_len += 32 * 1024;
			  symb = (char *) bfd_realloc_or_free (symb, buf_len);
			  if (symb == NULL)
			    goto error_return;
			  symb_rover = symb + num_chars;
			}
		      * symb_rover ++ = * str;
		      sum_chars += *str;
		      num_chars ++;
		      if (*str == '(')
			{
			  /* Skip the file number.  */
			  ++str;
			  while (ISDIGIT (*str))
			    ++str;
			  --str;
			}
		    }
		}
	    }

	  BFD_ASSERT (num_chars == (bfd_vma) (symb_rover - symb));

	  /* If we have already included a header file with the same
	     value, then replaced this one with an N_EXCL symbol.  */
	  incl_entry = (struct stab_link_includes_entry * )
	    bfd_hash_lookup (&sinfo->includes, string, true, true);
	  if (incl_entry == NULL)
	    goto error_return;

	  for (t = incl_entry->totals; t != NULL; t = t->next)
	    if (t->sum_chars == sum_chars
		&& t->num_chars == num_chars
		&& memcmp (t->symb, symb, num_chars) == 0)
	      break;

	  /* Record this symbol, so that we can set the value
	     correctly.  */
	  amt = sizeof *ne;
	  ne = (struct stab_excl_list *) bfd_alloc (abfd, amt);
	  if (ne == NULL)
	    goto error_return;
	  ne->offset = sym - stabbuf;
	  ne->val = sum_chars;
	  ne->type = (int) N_BINCL;
	  ne->next = secinfo->excls;
	  secinfo->excls = ne;

	  if (t == NULL)
	    {
	      /* This is the first time we have seen this header file
		 with this set of stabs strings.  */
	      t = (struct stab_link_includes_totals *)
		  bfd_hash_allocate (&sinfo->includes, sizeof *t);
	      if (t == NULL)
		goto error_return;
	      t->sum_chars = sum_chars;
	      t->num_chars = num_chars;
	      /* Trim data down.  */
	      t->symb = symb = (char *) bfd_realloc_or_free (symb, num_chars);
	      t->next = incl_entry->totals;
	      incl_entry->totals = t;
	    }
	  else
	    {
	      bfd_size_type *incl_pstridx;

	      /* We have seen this header file before.  Tell the final
		 pass to change the type to N_EXCL.  */
	      ne->type = (int) N_EXCL;

	      /* Free off superfluous symbols.  */
	      free (symb);

	      /* Mark the skipped symbols.  */

	      nest = 0;
	      for (incl_sym = sym + STABSIZE, incl_pstridx = pstridx + 1;
		   incl_sym < symend;
		   incl_sym += STABSIZE, ++incl_pstridx)
		{
		  int incl_type;

		  incl_type = incl_sym[TYPEOFF];

		  if (incl_type == (int) N_EINCL)
		    {
		      if (nest == 0)
			{
			  *incl_pstridx = (bfd_size_type) -1;
			  ++skip;
			  break;
			}
		      --nest;
		    }
		  else if (incl_type == (int) N_BINCL)
		    ++nest;
		  else if (incl_type == (int) N_EXCL)
		    /* Keep existing exclusion marks.  */
		    continue;
		  else if (nest == 0)
		    {
		      *incl_pstridx = (bfd_size_type) -1;
		      ++skip;
		    }
		}
	    }
	}
    }

  free (stabbuf);
  stabbuf = NULL;
  free (stabstrbuf);
  stabstrbuf = NULL;

  /* We need to set the section sizes such that the linker will
     compute the output section sizes correctly.  We set the .stab
     size to not include the entries we don't want.  We set
     SEC_EXCLUDE for the .stabstr section, so that it will be dropped
     from the link.  We record the size of the strtab in the first
     .stabstr section we saw, and make sure we don't set SEC_EXCLUDE
     for that section.  */
  stabsec->size = (count - skip) * STABSIZE;
  if (stabsec->size == 0)
    stabsec->flags |= SEC_EXCLUDE | SEC_KEEP;
  stabstrsec->flags |= SEC_EXCLUDE | SEC_KEEP;
  sinfo->stabstr->size = _bfd_stringtab_size (sinfo->strings);

  /* Calculate the `cumulative_skips' array now that stabs have been
     deleted for this section.  */

  if (skip != 0)
    {
      bfd_size_type i, offset;
      bfd_size_type *pskips;

      amt = count * sizeof (bfd_size_type);
      secinfo->cumulative_skips = (bfd_size_type *) bfd_alloc (abfd, amt);
      if (secinfo->cumulative_skips == NULL)
	goto error_return;

      pskips = secinfo->cumulative_skips;
      pstridx = secinfo->stridxs;
      offset = 0;

      for (i = 0; i < count; i++, pskips++, pstridx++)
	{
	  *pskips = offset;
	  if (*pstridx == (bfd_size_type) -1)
	    offset += STABSIZE;
	}

      BFD_ASSERT (offset != 0);
    }

  return true;

 error_return:
  free (stabbuf);
  free (stabstrbuf);
  return false;
}

/*
INTERNAL_FUNCTION
	_bfd_discard_section_stabs

SYNOPSIS
	bool _bfd_discard_section_stabs
	  (bfd *, asection *, void *, bool (*) (bfd_vma, void *), void *);

DESCRIPTION
	This function is called for each input file before the stab
	section is relocated.  It discards stab entries for discarded
	functions and variables.  The function returns TRUE iff
	any entries have been deleted.
*/

bool
_bfd_discard_section_stabs (bfd *abfd,
			    asection *stabsec,
			    void * psecinfo,
			    bool (*reloc_symbol_deleted_p) (bfd_vma, void *),
			    void * cookie)
{
  bfd_size_type count, amt;
  struct stab_section_info *secinfo;
  bfd_byte *stabbuf = NULL;
  bfd_byte *sym, *symend;
  bfd_size_type skip;
  bfd_size_type *pstridx;
  int deleting;

  if (stabsec->size == 0 || (stabsec->flags & SEC_HAS_CONTENTS) == 0)
    /* This file does not contain stabs debugging information.  */
    return false;

  if (stabsec->size % STABSIZE != 0)
    /* Something is wrong with the format of these stab symbols.
       Don't try to optimize them.  */
    return false;

  if ((stabsec->output_section != NULL
       && bfd_is_abs_section (stabsec->output_section)))
    /* At least one of the sections is being discarded from the
       link, so we should just ignore them.  */
    return false;

  /* We should have initialized our data in _bfd_link_section_stabs.
     If there was some bizarre error reading the string sections, though,
     we might not have.  Bail rather than asserting.  */
  if (psecinfo == NULL)
    return false;

  count = stabsec->rawsize / STABSIZE;
  secinfo = (struct stab_section_info *) psecinfo;

  /* Read the stabs information from abfd.  */
  if (!bfd_malloc_and_get_section (abfd, stabsec, &stabbuf))
    goto error_return;

  /* Look through the stabs symbols and discard any information for
     discarded functions.  */
  skip = 0;
  deleting = -1;

  symend = stabbuf + stabsec->rawsize;
  for (sym = stabbuf, pstridx = secinfo->stridxs;
       sym < symend;
       sym += STABSIZE, ++pstridx)
    {
      int type;

      if (*pstridx == (bfd_size_type) -1)
	/* This stab was deleted in a previous pass.  */
	continue;

      type = sym[TYPEOFF];

      if (type == (int) N_FUN)
	{
	  int strx = bfd_get_32 (abfd, sym + STRDXOFF);

	  if (strx == 0)
	    {
	      if (deleting)
		{
		  skip++;
		  *pstridx = -1;
		}
	      deleting = -1;
	      continue;
	    }
	  deleting = 0;
	  if ((*reloc_symbol_deleted_p) (sym + VALOFF - stabbuf, cookie))
	    deleting = 1;
	}

      if (deleting == 1)
	{
	  *pstridx = -1;
	  skip++;
	}
      else if (deleting == -1)
	{
	  /* Outside of a function.  Check for deleted variables.  */
	  if (type == (int) N_STSYM || type == (int) N_LCSYM)
	    if ((*reloc_symbol_deleted_p) (sym + VALOFF - stabbuf, cookie))
	      {
		*pstridx = -1;
		skip ++;
	      }
	  /* We should also check for N_GSYM entries which reference a
	     deleted global, but those are less harmful to debuggers
	     and would require parsing the stab strings.  */
	}
    }

  free (stabbuf);
  stabbuf = NULL;

  /* Shrink the stabsec as needed.  */
  stabsec->size -= skip * STABSIZE;
  if (stabsec->size == 0)
    stabsec->flags |= SEC_EXCLUDE | SEC_KEEP;

  /* Recalculate the `cumulative_skips' array now that stabs have been
     deleted for this section.  */

  if (skip != 0)
    {
      bfd_size_type i, offset;
      bfd_size_type *pskips;

      if (secinfo->cumulative_skips == NULL)
	{
	  amt = count * sizeof (bfd_size_type);
	  secinfo->cumulative_skips = (bfd_size_type *) bfd_alloc (abfd, amt);
	  if (secinfo->cumulative_skips == NULL)
	    goto error_return;
	}

      pskips = secinfo->cumulative_skips;
      pstridx = secinfo->stridxs;
      offset = 0;

      for (i = 0; i < count; i++, pskips++, pstridx++)
	{
	  *pskips = offset;
	  if (*pstridx == (bfd_size_type) -1)
	    offset += STABSIZE;
	}

      BFD_ASSERT (offset != 0);
    }

  return skip > 0;

 error_return:
  free (stabbuf);
  return false;
}

/*
INTERNAL_FUNCTION
	_bfd_write_section_stabs

SYNOPSIS
	bool _bfd_write_section_stabs
	  (bfd *, struct stab_info *, asection *, void **, bfd_byte *);

DESCRIPTION
	Write out the stab section.  This is called with the relocated
	contents.
*/

bool
_bfd_write_section_stabs (bfd *output_bfd,
			  struct stab_info *sinfo,
			  asection *stabsec,
			  void * *psecinfo,
			  bfd_byte *contents)
{
  struct stab_section_info *secinfo;
  struct stab_excl_list *e;
  bfd_byte *sym, *tosym, *symend;
  bfd_size_type *pstridx;

  secinfo = (struct stab_section_info *) *psecinfo;

  if (secinfo == NULL)
    return bfd_set_section_contents (output_bfd, stabsec->output_section,
				     contents, stabsec->output_offset,
				     stabsec->size);

  /* Handle each N_BINCL entry.  */
  for (e = secinfo->excls; e != NULL; e = e->next)
    {
      bfd_byte *excl_sym;

      BFD_ASSERT (e->offset < stabsec->rawsize);
      excl_sym = contents + e->offset;
      bfd_put_32 (output_bfd, e->val, excl_sym + VALOFF);
      excl_sym[TYPEOFF] = e->type;
    }

  /* Copy over all the stabs symbols, omitting the ones we don't want,
     and correcting the string indices for those we do want.  */
  tosym = contents;
  symend = contents + stabsec->rawsize;
  for (sym = contents, pstridx = secinfo->stridxs;
       sym < symend;
       sym += STABSIZE, ++pstridx)
    {
      if (*pstridx != (bfd_size_type) -1)
	{
	  if (tosym != sym)
	    memcpy (tosym, sym, STABSIZE);
	  bfd_put_32 (output_bfd, *pstridx, tosym + STRDXOFF);

	  if (sym[TYPEOFF] == 0)
	    {
	      /* This is the header symbol for the stabs section.  We
		 don't really need one, since we have merged all the
		 input stabs sections into one, but we generate one
		 for the benefit of readers which expect to see one.  */
	      BFD_ASSERT (sym == contents);
	      bfd_put_32 (output_bfd, _bfd_stringtab_size (sinfo->strings),
			  tosym + VALOFF);
	      bfd_put_16 (output_bfd,
			  stabsec->output_section->size / STABSIZE - 1,
			  tosym + DESCOFF);
	    }

	  tosym += STABSIZE;
	}
    }

  BFD_ASSERT ((bfd_size_type) (tosym - contents) == stabsec->size);

  return bfd_set_section_contents (output_bfd, stabsec->output_section,
				   contents, (file_ptr) stabsec->output_offset,
				   stabsec->size);
}

/*
INTERNAL_FUNCTION
	_bfd_write_stab_strings

SYNOPSIS
	bool _bfd_write_stab_strings (bfd *, struct stab_info *);

DESCRIPTION
	Write out the .stabstr section.
*/

bool
_bfd_write_stab_strings (bfd *output_bfd, struct stab_info *sinfo)
{
  if (bfd_is_abs_section (sinfo->stabstr->output_section))
    /* The section was discarded from the link.  */
    return true;

  BFD_ASSERT ((sinfo->stabstr->output_offset
	       + _bfd_stringtab_size (sinfo->strings))
	      <= sinfo->stabstr->output_section->size);

  if (bfd_seek (output_bfd,
		(file_ptr) (sinfo->stabstr->output_section->filepos
			    + sinfo->stabstr->output_offset),
		SEEK_SET) != 0)
    return false;

  if (! _bfd_stringtab_emit (output_bfd, sinfo->strings))
    return false;

  /* We no longer need the stabs information.  */
  _bfd_stringtab_free (sinfo->strings);
  bfd_hash_table_free (&sinfo->includes);

  return true;
}

/*
INTERNAL_FUNCTION
	_bfd_stab_section_offset

SYNOPSIS
	bfd_vma _bfd_stab_section_offset (asection *, void *, bfd_vma);

DESCRIPTION
	Adjust an address in the .stab section.  Given OFFSET within
	STABSEC, this returns the new offset in the adjusted stab section,
	or -1 if the address refers to a stab which has been removed.
*/

bfd_vma
_bfd_stab_section_offset (asection *stabsec,
			  void * psecinfo,
			  bfd_vma offset)
{
  struct stab_section_info *secinfo;

  secinfo = (struct stab_section_info *) psecinfo;

  if (secinfo == NULL)
    return offset;

  if (offset >= stabsec->rawsize)
    return offset - stabsec->rawsize + stabsec->size;

  if (secinfo->cumulative_skips)
    {
      bfd_vma i;

      i = offset / STABSIZE;

      if (secinfo->stridxs [i] == (bfd_size_type) -1)
	return (bfd_vma) -1;

      return offset - secinfo->cumulative_skips [i];
    }

  return offset;
}

/* stabs.c -- Parse stabs debugging information
   Copyright (C) 1995-2023 Free Software Foundation, Inc.
   Written by Ian Lance Taylor <ian@cygnus.com>.

   This file is part of GNU Binutils.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

   /* This file contains code which parses stabs debugging information.
	  The organization of this code is based on the gdb stabs reading
	  code.  The job it does is somewhat different, because it is not
	  trying to identify the correct address for anything.  */

#include "sysdep.h"
#include "bfd.h"
#include "libiberty.h"
#include "safe-ctype.h"
#include "demangle.h"
#include "debug.h"
#include "budbg.h"
#include "filenames.h"
#include "aout/aout64.h"
#include "aout/stab_gnu.h"

	  /* The number of predefined XCOFF types.  */

#define XCOFF_TYPE_COUNT 34

/* This structure is used as a handle so that the stab parsing doesn't
   need to use any static variables.  */

struct stab_handle
{
	/* The BFD.  */
	bfd* abfd;
	/* TRUE if this is stabs in sections.  */
	bool sections;
	/* The symbol table.  */
	asymbol** syms;
	/* The number of symbols.  */
	long long symcount;
	/* The accumulated file name string.  */
	char* so_string;
	/* The value of the last N_SO symbol.  */
	bfd_vma so_value;
	/* The value of the start of the file, so that we can handle file
	   relative N_LBRAC and N_RBRAC symbols.  */
	bfd_vma file_start_offset;
	/* The offset of the start of the function, so that we can handle
	   function relative N_LBRAC and N_RBRAC symbols.  */
	bfd_vma function_start_offset;
	/* The version number of gcc which compiled the current compilation
	   unit, 0 if not compiled by gcc.  */
	int gcc_compiled;
	/* Whether an N_OPT symbol was seen that was not generated by gcc,
	   so that we can detect the SunPRO compiler.  */
	bool n_opt_found;
	/* The main file name.  */
	char* main_filename;
	/* A stack of unfinished N_BINCL files.  */
	struct bincl_file* bincl_stack;
	/* A list of finished N_BINCL files.  */
	struct bincl_file* bincl_list;
	/* Whether we are inside a function or not.  */
	bool within_function;
	/* The address of the end of the function, used if we have seen an
	   N_FUN symbol while in a function.  This is -1 if we have not seen
	   an N_FUN (the normal case).  */
	bfd_vma function_end;
	/* The depth of block nesting.  */
	int block_depth;
	/* List of pending variable definitions.  */
	struct stab_pending_var* pending;
	/* Number of files for which we have types.  */
	unsigned int files;
	/* Lists of types per file.  */
	struct stab_types** file_types;
	/* Predefined XCOFF types.  */
	debug_type xcoff_types[XCOFF_TYPE_COUNT];
	/* Undefined tags.  */
	struct stab_tag* tags;
	/* Set by parse_stab_type if it sees a structure defined as a cross
	   reference to itself.  Reset by parse_stab_type otherwise.  */
	bool self_crossref;
};

/* A list of these structures is used to hold pending variable
   definitions seen before the N_LBRAC of a block.  */

struct stab_pending_var
{
	/* Next pending variable definition.  */
	struct stab_pending_var* next;
	/* Name.  */
	const char* name;
	/* Type.  */
	debug_type type;
	/* Kind.  */
	enum debug_var_kind kind;
	/* Value.  */
	bfd_vma val;
};

/* A list of these structures is used to hold the types for a single
   file.  */

struct stab_types
{
	/* Next set of slots for this file.  */
	struct stab_types* next;
	/* Where the TYPES array starts.  */
	unsigned int base_index;
	/* Types indexed by type number.  */
#define STAB_TYPES_SLOTS (16)
	debug_type types[STAB_TYPES_SLOTS];
};

/* We keep a list of undefined tags that we encounter, so that we can
   fill them in if the tag is later defined.  */

struct stab_tag
{
	/* Next undefined tag.  */
	struct stab_tag* next;
	/* Tag name.  */
	const char* name;
	/* Type kind.  */
	enum debug_type_kind kind;
	/* Slot to hold real type when we discover it.  If we don't, we fill
	   in an undefined tag type.  */
	debug_type slot;
	/* Indirect type we have created to point at slot.  */
	debug_type type;
};

static void bad_stab(const char*);
static void warn_stab(const char*, const char*);
static bool parse_stab_string
(void*, struct stab_handle*, int, int, bfd_vma,
	const char*, const char*);
static debug_type parse_stab_type
(void*, struct stab_handle*, const char*, const char**,
	debug_type**, const char*);
static bool parse_stab_type_number
(const char**, int*, const char*);
static debug_type parse_stab_range_type
(void*, struct stab_handle*, const char*, const char**,
	const int*, const char*);
static debug_type parse_stab_sun_builtin_type
(void*, const char**, const char*);
static debug_type parse_stab_sun_floating_type
(void*, const char**, const char*);
static debug_type parse_stab_enum_type
(void*, const char**, const char*);
static debug_type parse_stab_struct_type
(void*, struct stab_handle*, const char*, const char**,
	bool, const int*, const char*);
static bool parse_stab_baseclasses
(void*, struct stab_handle*, const char**, debug_baseclass**,
	const char*);
static bool parse_stab_struct_fields
(void*, struct stab_handle*, const char**, debug_field**,
	bool*, const char*);
static bool parse_stab_cpp_abbrev
(void*, struct stab_handle*, const char**, debug_field*, const char*);
static bool parse_stab_one_struct_field
(void*, struct stab_handle*, const char**, const char*,
	debug_field*, bool*, const char*);
static bool parse_stab_members
(void*, struct stab_handle*, const char*, const char**, const int*,
	debug_method**, const char*);
static debug_type parse_stab_argtypes
(void*, struct stab_handle*, debug_type, const char*, const char*,
	debug_type, const char*, bool, bool, const char**);
static bool parse_stab_tilde_field
(void*, struct stab_handle*, const char**, const int*, debug_type*,
	bool*, const char*);
static debug_type parse_stab_array_type
(void*, struct stab_handle*, const char**, bool, const char*);
static void push_bincl(void*, struct stab_handle*, const char*, bfd_vma);
static const char* pop_bincl(struct stab_handle*);
static bool find_excl(struct stab_handle*, const char*, bfd_vma);
static bool stab_record_variable
(void*, struct stab_handle*, const char*, debug_type,
	enum debug_var_kind, bfd_vma);
static bool stab_emit_pending_vars(void*, struct stab_handle*);
static debug_type* stab_find_slot(void*, struct stab_handle*, const int*);
static debug_type stab_find_type(void*, struct stab_handle*, const int*);
static bool stab_record_type
(void*, struct stab_handle*, const int*, debug_type);
static debug_type stab_xcoff_builtin_type
(void*, struct stab_handle*, unsigned int);
static debug_type stab_find_tagged_type
(void*, struct stab_handle*, const char*, int, enum debug_type_kind);
static debug_type* stab_demangle_argtypes
(void*, struct stab_handle*, const char*, bool*, unsigned int);
static debug_type* stab_demangle_v3_argtypes
(void*, struct stab_handle*, const char*, bool*);
static debug_type* stab_demangle_v3_arglist
(void*, struct stab_handle*, struct demangle_component*, bool*);
static debug_type stab_demangle_v3_arg
(void*, struct stab_handle*, struct demangle_component*, debug_type,
	bool*);

static int demangle_flags = DMGL_ANSI;

/* Save a string in memory.  */

static char*
savestring(void* dhandle, const char* start, size_t len)
{
	char* ret;

	ret = debug_xalloc(dhandle, len + 1);
	memcpy(ret, start, len);
	ret[len] = '\0';
	return ret;
}

/* Read a number from a string.  */

static bfd_vma
parse_number(const char** pp, bool* poverflow, const char* p_end)
{
	unsigned long long ul;
	const char* orig;

	if (poverflow != NULL)
		*poverflow = false;

	orig = *pp;
	if (orig >= p_end)
		return (bfd_vma)0;

	/* Stop early if we are passed an empty string.  */
	if (*orig == 0)
		return (bfd_vma)0;

	errno = 0;
	ul = strtoul(*pp, (char**)pp, 0);
	if (ul + 1 != 0 || errno == 0)
	{
		/* If bfd_vma is larger than unsigned long long, and the number is
		   meant to be negative, we have to make sure that we sign
		   extend properly.  */
		if (*orig == '-')
			return (bfd_vma)(bfd_signed_vma)(long long)ul;
		return (bfd_vma)ul;
	}

	/* Note that even though strtoul overflowed, it should have set *pp
	   to the end of the number, which is where we want it.  */
	if (sizeof(bfd_vma) > sizeof(unsigned long long))
	{
		const char* p;
		bool neg;
		int base;
		bfd_vma over, lastdig;
		bool overflow;
		bfd_vma v;

		/* Our own version of strtoul, for a bfd_vma.  */
		p = orig;

		neg = false;
		if (*p == '+')
			++p;
		else if (*p == '-')
		{
			neg = true;
			++p;
		}

		base = 10;
		if (*p == '0')
		{
			if (p[1] == 'x' || p[1] == 'X')
			{
				base = 16;
				p += 2;
			}
			else
			{
				base = 8;
				++p;
			}
		}

		over = ((bfd_vma)(bfd_signed_vma)-1) / (bfd_vma)base;
		lastdig = ((bfd_vma)(bfd_signed_vma)-1) % (bfd_vma)base;

		overflow = false;
		v = 0;
		while (1)
		{
			int d;

			d = *p++;
			if (ISDIGIT(d))
				d -= '0';
			else if (ISUPPER(d))
				d -= 'A';
			else if (ISLOWER(d))
				d -= 'a';
			else
				break;

			if (d >= base)
				break;

			if (v > over || (v == over && (bfd_vma)d > lastdig))
			{
				overflow = true;
				break;
			}
		}

		if (!overflow)
		{
			if (neg)
				v = -v;
			return v;
		}
	}

	/* If we get here, the number is too large to represent in a
	   bfd_vma.  */
	if (poverflow != NULL)
		*poverflow = true;
	else
		warn_stab(orig, _("numeric overflow"));

	return 0;
}

/* Give an error for a bad stab string.  */

static void
bad_stab(const char* p)
{
	fprintf(stderr, _("Bad stab: %s\n"), p);
}

/* Warn about something in a stab string.  */

static void
warn_stab(const char* p, const char* err)
{
	fprintf(stderr, _("Warning: %s: %s\n"), err, p);
}

/* Create a handle to parse stabs symbols with.  */

void*
start_stab(void* dhandle ATTRIBUTE_UNUSED, bfd* abfd, bool sections,
	asymbol** syms, long long symcount)
{
	struct stab_handle* ret;

	ret = xmalloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));
	ret->abfd = abfd;
	ret->sections = sections;
	ret->syms = syms;
	ret->symcount = symcount;
	ret->files = 1;
	ret->file_types = xmalloc(sizeof(*ret->file_types));
	ret->file_types[0] = NULL;
	ret->function_end = -1;
	return ret;
}

/* When we have processed all the stabs information, we need to go
   through and fill in all the undefined tags.  */

bool
finish_stab(void* dhandle, void* handle, bool emit)
{
	struct stab_handle* info = (struct stab_handle*)handle;
	struct stab_tag* st;
	bool ret = true;

	if (emit && info->within_function)
	{
		if (!stab_emit_pending_vars(dhandle, info)
			|| !debug_end_function(dhandle, info->function_end))
			ret = false;
	}

	if (emit && ret)
		for (st = info->tags; st != NULL; st = st->next)
		{
			enum debug_type_kind kind;

			kind = st->kind;
			if (kind == DEBUG_KIND_ILLEGAL)
				kind = DEBUG_KIND_STRUCT;
			st->slot = debug_make_undefined_tagged_type(dhandle, st->name, kind);
			if (st->slot == DEBUG_TYPE_NULL)
			{
				ret = false;
				break;
			}
		}

	free(info->file_types);
	free(info->so_string);
	free(info);
	return ret;
}

/* Handle a single stabs symbol.  */

bool
parse_stab(void* dhandle, void* handle, int type, int desc, bfd_vma value,
	const char* string)
{
	const char* string_end;
	struct stab_handle* info = (struct stab_handle*)handle;
	char* copy;
	size_t len;

	/* gcc will emit two N_SO strings per compilation unit, one for the
	   directory name and one for the file name.  We just collect N_SO
	   strings as we see them, and start the new compilation unit when
	   we see a non N_SO symbol.  */
	if (info->so_string != NULL
		&& (type != N_SO || *string == '\0' || value != info->so_value))
	{
		len = strlen(info->so_string) + 1;
		copy = debug_xalloc(dhandle, len);
		memcpy(copy, info->so_string, len);
		if (!debug_set_filename(dhandle, copy))
			return false;
		info->main_filename = copy;

		free(info->so_string);
		info->so_string = NULL;

		info->gcc_compiled = 0;
		info->n_opt_found = false;

		/* Generally, for stabs in the symbol table, the N_LBRAC and
	   N_RBRAC symbols are relative to the N_SO symbol value.  */
		if (!info->sections)
			info->file_start_offset = info->so_value;

		/* We need to reset the mapping from type numbers to types.  We
	   can only free the file_types array, not the stab_types
	   list entries due to the use of debug_make_indirect_type.  */
		info->files = 1;
		info->file_types = xrealloc(info->file_types, sizeof(*info->file_types));
		info->file_types[0] = NULL;

		/* Now process whatever type we just got.  */
	}

	string_end = string + strlen(string);

	switch (type)
	{
	case N_FN:
	case N_FN_SEQ:
		break;

	case N_LBRAC:
		/* Ignore extra outermost context from SunPRO cc and acc.  */
		if (info->n_opt_found && desc == 1)
			break;

		if (!info->within_function)
		{
			fprintf(stderr, _("N_LBRAC not within function\n"));
			return false;
		}

		/* Start an inner lexical block.  */
		if (!debug_start_block(dhandle,
			(value
				+ info->file_start_offset
				+ info->function_start_offset)))
			return false;

		/* Emit any pending variable definitions.  */
		if (!stab_emit_pending_vars(dhandle, info))
			return false;

		++info->block_depth;
		break;

	case N_RBRAC:
		/* Ignore extra outermost context from SunPRO cc and acc.  */
		if (info->n_opt_found && desc == 1)
			break;

		/* We shouldn't have any pending variable definitions here, but,
		   if we do, we probably need to emit them before closing the
		   block.  */
		if (!stab_emit_pending_vars(dhandle, info))
			return false;

		/* End an inner lexical block.  */
		if (!debug_end_block(dhandle,
			(value
				+ info->file_start_offset
				+ info->function_start_offset)))
			return false;

		--info->block_depth;
		if (info->block_depth < 0)
		{
			fprintf(stderr, _("Too many N_RBRACs\n"));
			return false;
		}
		break;

	case N_SO:
		/* This always ends a function.  */
		if (info->within_function)
		{
			bfd_vma endval;

			endval = value;
			if (*string != '\0'
				&& info->function_end != (bfd_vma)-1
				&& info->function_end < endval)
				endval = info->function_end;
			if (!stab_emit_pending_vars(dhandle, info)
				|| !debug_end_function(dhandle, endval))
				return false;
			info->within_function = false;
			info->function_end = (bfd_vma)-1;
		}

		/* An empty string is emitted by gcc at the end of a compilation
		   unit.  */
		if (*string == '\0')
			return true;

		/* Just accumulate strings until we see a non N_SO symbol.  If
		   the string starts with a directory separator or some other
	   form of absolute path specification, we discard the previously
		   accumulated strings.  */
		if (info->so_string == NULL)
			info->so_string = xstrdup(string);
		else
		{
			char* f;

			f = info->so_string;

			if (IS_ABSOLUTE_PATH(string))
				info->so_string = xstrdup(string);
			else
				info->so_string = concat(info->so_string, string,
					(const char*)NULL);
			free(f);
		}

		info->so_value = value;

		break;

	case N_SOL:
		/* Start an include file.  */
		len = strlen(string) + 1;
		copy = debug_xalloc(dhandle, len);
		memcpy(copy, string, len);
		if (!debug_start_source(dhandle, copy))
			return false;
		break;

	case N_BINCL:
		/* Start an include file which may be replaced.  */
		len = strlen(string) + 1;
		copy = debug_xalloc(dhandle, len);
		memcpy(copy, string, len);
		push_bincl(dhandle, info, copy, value);
		if (!debug_start_source(dhandle, copy))
			return false;
		break;

	case N_EINCL:
		/* End an N_BINCL include.  */
		if (!debug_start_source(dhandle, pop_bincl(info)))
			return false;
		break;

	case N_EXCL:
		/* This is a duplicate of a header file named by N_BINCL which
		   was eliminated by the linker.  */
		if (!find_excl(info, string, value))
			return false;
		break;

	case N_SLINE:
		if (!debug_record_line(dhandle, desc,
			value + (info->within_function
				? info->function_start_offset : 0)))
			return false;
		break;

	case N_BCOMM:
		if (!debug_start_common_block(dhandle, string))
			return false;
		break;

	case N_ECOMM:
		if (!debug_end_common_block(dhandle, string))
			return false;
		break;

	case N_FUN:
		if (*string == '\0')
		{
			if (info->within_function)
			{
				/* This always marks the end of a function; we don't
					   need to worry about info->function_end.  */
				if (info->sections)
					value += info->function_start_offset;
				if (!stab_emit_pending_vars(dhandle, info)
					|| !debug_end_function(dhandle, value))
					return false;
				info->within_function = false;
				info->function_end = (bfd_vma)-1;
			}
			break;
		}

		/* A const static symbol in the .text section will have an N_FUN
		   entry.  We need to use these to mark the end of the function,
		   in case we are looking at gcc output before it was changed to
		   always emit an empty N_FUN.  We can't call debug_end_function
		   here, because it might be a local static symbol.  */
		if (info->within_function
			&& (info->function_end == (bfd_vma)-1
				|| value < info->function_end))
			info->function_end = value;

		/* Fall through.  */
		/* FIXME: gdb checks the string for N_STSYM, N_LCSYM or N_ROSYM
		   symbols, and if it does not start with :S, gdb relocates the
		   value to the start of the section.  gcc always seems to use
		   :S, so we don't worry about this.  */
		   /* Fall through.  */
	default:
	{
		const char* colon;

		colon = strchr(string, ':');
		if (colon != NULL
			&& (colon[1] == 'f' || colon[1] == 'F'))
		{
			if (info->within_function)
			{
				bfd_vma endval;

				endval = value;
				if (info->function_end != (bfd_vma)-1
					&& info->function_end < endval)
					endval = info->function_end;
				if (!stab_emit_pending_vars(dhandle, info)
					|| !debug_end_function(dhandle, endval))
					return false;
				info->function_end = (bfd_vma)-1;
			}
			/* For stabs in sections, line numbers and block addresses
				   are offsets from the start of the function.  */
			if (info->sections)
				info->function_start_offset = value;
			info->within_function = true;
		}

		if (!parse_stab_string(dhandle, info, type, desc, value, string, string_end))
			return false;
	}
	break;

	case N_OPT:
		if (string != NULL && strcmp(string, "gcc2_compiled.") == 0)
			info->gcc_compiled = 2;
		else if (string != NULL && strcmp(string, "gcc_compiled.") == 0)
			info->gcc_compiled = 1;
		else
			info->n_opt_found = true;
		break;

	case N_OBJ:
	case N_ENDM:
	case N_MAIN:
	case N_WARNING:
		break;
	}

	return true;
}

/* Parse the stabs string.  */

static bool
parse_stab_string(void* dhandle, struct stab_handle* info, int stabtype,
	int desc ATTRIBUTE_UNUSED, bfd_vma value,
	const char* string, const char* string_end)
{
	const char* p;
	char* name;
	int type;
	debug_type dtype;
	bool synonym;
	bool self_crossref;
	debug_type* slot;

	p = strchr(string, ':');
	if (p == NULL)
		return true;

	while (p[1] == ':')
	{
		p += 2;
		p = strchr(p, ':');
		if (p == NULL)
		{
			bad_stab(string);
			return false;
		}
	}

	/* FIXME: Sometimes the special C++ names start with '.'.  */
	name = NULL;
	if (string[0] == '$')
	{
		switch (string[1])
		{
		case 't':
			name = "this";
			break;
		case 'v':
			/* Was: name = "vptr"; */
			break;
		case 'e':
			name = "eh_throw";
			break;
		case '_':
			/* This was an anonymous type that was never fixed up.  */
			break;
		case 'X':
			/* SunPRO (3.0 at least) static variable encoding.  */
			break;
		default:
			warn_stab(string, _("unknown C++ encoded name"));
			break;
		}
	}

	if (name == NULL)
	{
		if (p == string || (string[0] == ' ' && p == string + 1))
			name = NULL;
		else
			name = savestring(dhandle, string, p - string);
	}

	++p;
	if (ISDIGIT(*p) || *p == '(' || *p == '-')
		type = 'l';
	else if (*p == 0)
	{
		bad_stab(string);
		return false;
	}
	else
		type = *p++;

	switch (type)
	{
	case 'c':
		/* c is a special case, not followed by a type-number.
	   SYMBOL:c=iVALUE for an integer constant symbol.
	   SYMBOL:c=rVALUE for a floating constant symbol.
	   SYMBOL:c=eTYPE,INTVALUE for an enum constant symbol.
	   e.g. "b:c=e6,0" for "const b = blob1"
	   (where type 6 is defined by "blobs:t6=eblob1:0,blob2:1,;").  */
		if (*p != '=')
		{
			bad_stab(string);
			return false;
		}
		++p;
		switch (*p++)
		{
		case 'r':
			/* Floating point constant.  */
			if (!debug_record_float_const(dhandle, name, atof(p)))
				return false;
			break;
		case 'i':
			/* Integer constant.  */
			/* Defining integer constants this way is kind of silly,
			   since 'e' constants allows the compiler to give not only
			   the value, but the type as well.  C has at least int,
			   long long, unsigned int, and long long as constant types;
			   other languages probably should have at least unsigned as
			   well as signed constants.  */
			if (!debug_record_int_const(dhandle, name, atoi(p)))
				return false;
			break;
		case 'e':
			/* SYMBOL:c=eTYPE,INTVALUE for a constant symbol whose value
			   can be represented as integral.
			   e.g. "b:c=e6,0" for "const b = blob1"
			   (where type 6 is defined by "blobs:t6=eblob1:0,blob2:1,;").  */
			dtype = parse_stab_type(dhandle, info, (const char*)NULL,
				&p, (debug_type**)NULL, string_end);
			if (dtype == DEBUG_TYPE_NULL)
				return false;
			if (*p != ',')
			{
				bad_stab(string);
				return false;
			}
			if (!debug_record_typed_const(dhandle, name, dtype, atoi(p)))
				return false;
			break;
		default:
			bad_stab(string);
			return false;
		}

		break;

	case 'C':
		/* The name of a caught exception.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL,
			&p, (debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!debug_record_label(dhandle, name, dtype, value))
			return false;
		break;

	case 'f':
	case 'F':
		/* A function definition.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!debug_record_function(dhandle, name, dtype, type == 'F', value))
			return false;

		/* Sun acc puts declared types of arguments here.  We don't care
	   about their actual types (FIXME -- we should remember the whole
	   function prototype), but the list may define some new types
	   that we have to remember, so we must scan it now.  */
		while (*p == ';')
		{
			++p;
			if (parse_stab_type(dhandle, info, (const char*)NULL, &p,
				(debug_type**)NULL, string_end)
				== DEBUG_TYPE_NULL)
				return false;
		}

		break;

	case 'G':
	{
		asymbol** ps;

		/* A global symbol.  The value must be extracted from the
		   symbol table.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (name != NULL)
		{
			char leading;
			long long c;

			leading = bfd_get_symbol_leading_char(info->abfd);
			for (c = info->symcount, ps = info->syms; c > 0; --c, ++ps)
			{
				const char* n;

				n = bfd_asymbol_name(*ps);
				if (leading != '\0' && *n == leading)
					++n;
				if (*n == *name && strcmp(n, name) == 0)
					break;
			}

			if (c > 0)
				value = bfd_asymbol_value(*ps);
		}

		if (!stab_record_variable(dhandle, info, name, dtype, DEBUG_GLOBAL,
			value))
			return false;
	}
	break;

	/* This case is faked by a conditional above, when there is no
   code letter in the dbx data.  Dbx data never actually
   contains 'l'.  */
	case 'l':
	case 's':
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!stab_record_variable(dhandle, info, name, dtype, DEBUG_LOCAL,
			value))
			return false;
		break;

	case 'p':
		/* A function parameter.  */
		if (*p != 'F')
			dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
				(debug_type**)NULL, string_end);
		else
		{
			/* pF is a two-letter code that means a function parameter in
			   Fortran.  The type-number specifies the type of the return
			   value.  Translate it into a pointer-to-function type.  */
			++p;
			dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
				(debug_type**)NULL, string_end);
			if (dtype != DEBUG_TYPE_NULL)
			{
				debug_type ftype;

				ftype = debug_make_function_type(dhandle, dtype,
					(debug_type*)NULL, false);
				dtype = debug_make_pointer_type(dhandle, ftype);
			}
		}
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!debug_record_parameter(dhandle, name, dtype, DEBUG_PARM_STACK,
			value))
			return false;

		/* FIXME: At this point gdb considers rearranging the parameter
	   address on a big endian machine if it is smaller than an int.
	   We have no way to do that, since we don't really know much
	   about the target.  */
		break;

	case 'P':
		if (stabtype == N_FUN)
		{
			/* Prototype of a function referenced by this file.  */
			while (*p == ';')
			{
				++p;
				if (parse_stab_type(dhandle, info, (const char*)NULL, &p,
					(debug_type**)NULL, string_end)
					== DEBUG_TYPE_NULL)
					return false;
			}
			break;
		}
		/* Fall through.  */
	case 'R':
		/* Parameter which is in a register.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!debug_record_parameter(dhandle, name, dtype, DEBUG_PARM_REG,
			value))
			return false;
		break;

	case 'r':
		/* Register variable (either global or local).  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!stab_record_variable(dhandle, info, name, dtype, DEBUG_REGISTER,
			value))
			return false;

		/* FIXME: At this point gdb checks to combine pairs of 'p' and
	   'r' stabs into a single 'P' stab.  */
		break;

	case 'S':
		/* Static symbol at top level of file.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!stab_record_variable(dhandle, info, name, dtype, DEBUG_STATIC,
			value))
			return false;
		break;

	case 't':
		/* A typedef.  */
		dtype = parse_stab_type(dhandle, info, name, &p, &slot, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (name == NULL)
		{
			/* A nameless type.  Nothing to do.  */
			return true;
		}

		dtype = debug_name_type(dhandle, name, dtype);
		if (dtype == DEBUG_TYPE_NULL)
			return false;

		if (slot != NULL)
			*slot = dtype;

		break;

	case 'T':
		/* Struct, union, or enum tag.  For GNU C++, this can be followed
	   by 't' which means we are typedef'ing it as well.  */
		if (*p != 't')
		{
			synonym = false;
			/* FIXME: gdb sets synonym to TRUE if the current language
				   is C++.  */
		}
		else
		{
			synonym = true;
			++p;
		}

		dtype = parse_stab_type(dhandle, info, name, &p, &slot, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (name == NULL)
			return true;

		/* INFO->SELF_CROSSREF is set by parse_stab_type if this type is
		   a cross reference to itself.  These are generated by some
		   versions of g++.  */
		self_crossref = info->self_crossref;

		dtype = debug_tag_type(dhandle, name, dtype);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (slot != NULL)
			*slot = dtype;

		/* See if we have a cross reference to this tag which we can now
		   fill in.  Avoid filling in a cross reference to ourselves,
		   because that would lead to circular debugging information.  */
		if (!self_crossref)
		{
			register struct stab_tag** pst;

			for (pst = &info->tags; *pst != NULL; pst = &(*pst)->next)
			{
				if ((*pst)->name[0] == name[0]
					&& strcmp((*pst)->name, name) == 0)
				{
					(*pst)->slot = dtype;
					*pst = (*pst)->next;
					break;
				}
			}
		}

		if (synonym)
		{
			dtype = debug_name_type(dhandle, name, dtype);
			if (dtype == DEBUG_TYPE_NULL)
				return false;

			if (slot != NULL)
				*slot = dtype;
		}

		break;

	case 'V':
		/* Static symbol of local scope */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		/* FIXME: gdb checks os9k_stabs here.  */
		if (!stab_record_variable(dhandle, info, name, dtype,
			DEBUG_LOCAL_STATIC, value))
			return false;
		break;

	case 'v':
		/* Reference parameter.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!debug_record_parameter(dhandle, name, dtype, DEBUG_PARM_REFERENCE,
			value))
			return false;
		break;

	case 'a':
		/* Reference parameter which is in a register.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!debug_record_parameter(dhandle, name, dtype, DEBUG_PARM_REF_REG,
			value))
			return false;
		break;

	case 'X':
		/* This is used by Sun FORTRAN for "function result value".
	   Sun claims ("dbx and dbxtool interfaces", 2nd ed)
	   that Pascal uses it too, but when I tried it Pascal used
	   "x:3" (local symbol) instead.  */
		dtype = parse_stab_type(dhandle, info, (const char*)NULL, &p,
			(debug_type**)NULL, string_end);
		if (dtype == DEBUG_TYPE_NULL)
			return false;
		if (!stab_record_variable(dhandle, info, name, dtype, DEBUG_LOCAL,
			value))
			return false;
		break;

	case 'Y':
		/* SUNPro C++ Namespace =Yn0.  */
		/* Skip the namespace mapping, as it is not used now.  */
		if (*p++ != 0 && *p++ == 'n' && *p++ == '0')
		{
			/* =Yn0name; */
			while (*p && *p != ';')
				++p;
			if (*p)
				return true;
		}
		/* TODO SUNPro C++ support:
		   Support default arguments after F,P parameters
		   Ya = Anonymous unions
		   YM,YD = Pointers to class members
		   YT,YI = Templates
		   YR = Run-time type information (RTTI)  */

		   /* Fall through.  */

	default:
		bad_stab(string);
		return false;
	}

	/* FIXME: gdb converts structure values to structure pointers in a
	   couple of cases, depending upon the target.  */

	return true;
}

/* Parse a stabs type.  The typename argument is non-NULL if this is a
   typedef or a tag definition.  The pp argument points to the stab
   string, and is updated.  The slotp argument points to a place to
   store the slot used if the type is being defined.  */

static debug_type
parse_stab_type(void* dhandle,
	struct stab_handle* info,
	const char* type_name,
	const char** pp,
	debug_type** slotp,
	const char* p_end)
{
	const char* orig;
	int typenums[2];
	int size;
	bool stringp;
	int descriptor;
	debug_type dtype;

	if (slotp != NULL)
		*slotp = NULL;

	orig = *pp;
	if (orig >= p_end)
		return DEBUG_TYPE_NULL;

	size = -1;
	stringp = false;

	info->self_crossref = false;

	/* Read type number if present.  The type number may be omitted.
	   for instance in a two-dimensional array declared with type
	   "ar1;1;10;ar1;1;10;4".  */
	if (!ISDIGIT(**pp) && **pp != '(' && **pp != '-')
	{
		/* 'typenums=' not present, type is anonymous.  Read and return
	   the definition, but don't put it in the type vector.  */
		typenums[0] = typenums[1] = -1;
	}
	else
	{
		if (!parse_stab_type_number(pp, typenums, p_end))
			return DEBUG_TYPE_NULL;

		if (**pp != '=')
			/* Type is not being defined here.  Either it already
			   exists, or this is a forward reference to it.  */
			return stab_find_type(dhandle, info, typenums);

		/* Only set the slot if the type is being defined.  This means
		   that the mapping from type numbers to types will only record
		   the name of the typedef which defines a type.  If we don't do
		   this, then something like
		   typedef int foo;
		   int i;
	   will record that i is of type foo.  Unfortunately, stabs
	   information is ambiguous about variable types.  For this code,
		   typedef int foo;
		   int i;
		   foo j;
	   the stabs information records both i and j as having the same
	   type.  This could be fixed by patching the compiler.  */
		if (slotp != NULL && typenums[0] >= 0 && typenums[1] >= 0)
			*slotp = stab_find_slot(dhandle, info, typenums);

		/* Type is being defined here.  */
		/* Skip the '='.  */
		++* pp;

		while (**pp == '@')
		{
			const char* p = *pp + 1;
			const char* attr;

			if (ISDIGIT(*p) || *p == '(' || *p == '-')
				/* Member type.  */
				break;

			/* Type attributes.  */
			attr = p;

			for (; *p != ';'; ++p)
			{
				if (*p == '\0')
				{
					bad_stab(orig);
					return DEBUG_TYPE_NULL;
				}
			}
			*pp = p + 1;

			switch (*attr)
			{
			case 's':
				size = atoi(attr + 1);
				size /= 8;  /* Size is in bits.  We store it in bytes.  */
				if (size <= 0)
					size = -1;
				break;

			case 'S':
				stringp = true;
				break;

			case 0:
				bad_stab(orig);
				return DEBUG_TYPE_NULL;

			default:
				/* Ignore unrecognized type attributes, so future
			   compilers can invent new ones.  */
				break;
			}
		}
	}

	descriptor = **pp;
	++* pp;

	switch (descriptor)
	{
	case 'x':
	{
		enum debug_type_kind code;
		const char* q1, * q2, * p;

		/* A cross reference to another type.  */
		switch (**pp)
		{
		case 's':
			code = DEBUG_KIND_STRUCT;
			break;
		case 'u':
			code = DEBUG_KIND_UNION;
			break;
		case 'e':
			code = DEBUG_KIND_ENUM;
			break;
		case 0:
			bad_stab(orig);
			return DEBUG_TYPE_NULL;

		default:
			/* Complain and keep going, so compilers can invent new
			   cross-reference types.  */
			warn_stab(orig, _("unrecognized cross reference type"));
			code = DEBUG_KIND_STRUCT;
			break;
		}
		++* pp;

		q1 = strchr(*pp, '<');
		p = strchr(*pp, ':');
		if (p == NULL)
		{
			bad_stab(orig);
			return DEBUG_TYPE_NULL;
		}
		if (q1 != NULL && p > q1 && p[1] == ':')
		{
			int nest = 0;

			for (q2 = q1; *q2 != '\0'; ++q2)
			{
				if (*q2 == '<')
					++nest;
				else if (*q2 == '>')
					--nest;
				else if (*q2 == ':' && nest == 0)
					break;
			}
			p = q2;
			if (*p != ':')
			{
				bad_stab(orig);
				return DEBUG_TYPE_NULL;
			}
		}

		/* Some versions of g++ can emit stabs like
			   fleep:T20=xsfleep:
		   which define structures in terms of themselves.  We need to
		   tell the caller to avoid building a circular structure.  */
		if (type_name != NULL
			&& strncmp(type_name, *pp, p - *pp) == 0
			&& type_name[p - *pp] == '\0')
			info->self_crossref = true;

		dtype = stab_find_tagged_type(dhandle, info, *pp, p - *pp, code);

		*pp = p + 1;
	}
	break;

	case '-':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '(':
	{
		const char* hold;
		int xtypenums[2];

		/* This type is defined as another type.  */
		(*pp)--;
		hold = *pp;

		/* Peek ahead at the number to detect void.  */
		if (!parse_stab_type_number(pp, xtypenums, p_end))
			return DEBUG_TYPE_NULL;

		if (typenums[0] == xtypenums[0] && typenums[1] == xtypenums[1])
		{
			/* This type is being defined as itself, which means that
				   it is void.  */
			dtype = debug_make_void_type(dhandle);
		}
		else
		{
			*pp = hold;

			/* Go back to the number and have parse_stab_type get it.
			   This means that we can deal with something like
			   t(1,2)=(3,4)=... which the Lucid compiler uses.  */
			dtype = parse_stab_type(dhandle, info, (const char*)NULL,
				pp, (debug_type**)NULL, p_end);
			if (dtype == DEBUG_TYPE_NULL)
				return DEBUG_TYPE_NULL;
		}

		if (typenums[0] != -1)
		{
			if (!stab_record_type(dhandle, info, typenums, dtype))
				return DEBUG_TYPE_NULL;
		}

		break;
	}

	case '*':
		dtype = debug_make_pointer_type(dhandle,
			parse_stab_type(dhandle, info,
				(const char*)NULL,
				pp,
				(debug_type**)NULL,
				p_end));
		break;

	case '&':
		/* Reference to another type.  */
		dtype = (debug_make_reference_type
		(dhandle,
			parse_stab_type(dhandle, info, (const char*)NULL, pp,
				(debug_type**)NULL, p_end)));
		break;

	case 'f':
		/* Function returning another type.  */
		/* FIXME: gdb checks os9k_stabs here.  */
		dtype = (debug_make_function_type
		(dhandle,
			parse_stab_type(dhandle, info, (const char*)NULL, pp,
				(debug_type**)NULL, p_end),
			(debug_type*)NULL, false));
		break;

	case 'k':
		/* Const qualifier on some type (Sun).  */
		/* FIXME: gdb accepts 'c' here if os9k_stabs.  */
		dtype = debug_make_const_type(dhandle,
			parse_stab_type(dhandle, info,
				(const char*)NULL,
				pp,
				(debug_type**)NULL,
				p_end));
		break;

	case 'B':
		/* Volatile qual on some type (Sun).  */
		/* FIXME: gdb accepts 'i' here if os9k_stabs.  */
		dtype = (debug_make_volatile_type
		(dhandle,
			parse_stab_type(dhandle, info, (const char*)NULL, pp,
				(debug_type**)NULL, p_end)));
		break;

	case '@':
		/* Offset (class & variable) type.  This is used for a pointer
		   relative to an object.  */
	{
		debug_type domain;
		debug_type memtype;

		/* Member type.  */

		domain = parse_stab_type(dhandle, info, (const char*)NULL, pp,
			(debug_type**)NULL, p_end);
		if (domain == DEBUG_TYPE_NULL)
			return DEBUG_TYPE_NULL;

		if (**pp != ',')
		{
			bad_stab(orig);
			return DEBUG_TYPE_NULL;
		}
		++* pp;

		memtype = parse_stab_type(dhandle, info, (const char*)NULL, pp,
			(debug_type**)NULL, p_end);
		if (memtype == DEBUG_TYPE_NULL)
			return DEBUG_TYPE_NULL;

		dtype = debug_make_offset_type(dhandle, domain, memtype);
	}
	break;

	case '#':
		/* Method (class & fn) type.  */
		if (**pp == '#')
		{
			debug_type return_type;

			++* pp;
			return_type = parse_stab_type(dhandle, info, (const char*)NULL,
				pp, (debug_type**)NULL, p_end);
			if (return_type == DEBUG_TYPE_NULL)
				return DEBUG_TYPE_NULL;
			if (**pp != ';')
			{
				bad_stab(orig);
				return DEBUG_TYPE_NULL;
			}
			++* pp;
			dtype = debug_make_method_type(dhandle, return_type,
				DEBUG_TYPE_NULL,
				(debug_type*)NULL, false);
		}
		else
		{
			debug_type domain;
			debug_type return_type;
			debug_type* args, * xargs;
			unsigned int n;
			unsigned int alloc;
			bool varargs;

			domain = parse_stab_type(dhandle, info, (const char*)NULL,
				pp, (debug_type**)NULL, p_end);
			if (domain == DEBUG_TYPE_NULL)
				return DEBUG_TYPE_NULL;

			if (**pp != ',')
			{
				bad_stab(orig);
				return DEBUG_TYPE_NULL;
			}
			++* pp;

			return_type = parse_stab_type(dhandle, info, (const char*)NULL,
				pp, (debug_type**)NULL, p_end);
			if (return_type == DEBUG_TYPE_NULL)
				return DEBUG_TYPE_NULL;

			alloc = 10;
			args = xmalloc(alloc * sizeof(*args));
			n = 0;
			while (**pp != ';')
			{
				if (**pp != ',')
				{
					bad_stab(orig);
					free(args);
					return DEBUG_TYPE_NULL;
				}
				++* pp;

				if (n + 1 >= alloc)
				{
					alloc += 10;
					args = xrealloc(args, alloc * sizeof(*args));
				}

				args[n] = parse_stab_type(dhandle, info, (const char*)NULL,
					pp, (debug_type**)NULL, p_end);
				if (args[n] == DEBUG_TYPE_NULL)
				{
					free(args);
					return DEBUG_TYPE_NULL;
				}
				++n;
			}
			++* pp;

			/* If the last type is not void, then this function takes a
			   variable number of arguments.  Otherwise, we must strip
			   the void type.  */
			if (n == 0
				|| debug_get_type_kind(dhandle, args[n - 1]) != DEBUG_KIND_VOID)
				varargs = true;
			else
			{
				--n;
				varargs = false;
			}

			args[n] = DEBUG_TYPE_NULL;
			xargs = debug_xalloc(dhandle, (n + 1) * sizeof(*args));
			memcpy(xargs, args, (n + 1) * sizeof(*args));
			free(args);

			dtype = debug_make_method_type(dhandle, return_type, domain, xargs,
				varargs);
		}
		break;

	case 'r':
		/* Range type.  */
		dtype = parse_stab_range_type(dhandle, info, type_name, pp, typenums, p_end);
		break;

	case 'b':
		/* FIXME: gdb checks os9k_stabs here.  */
		/* Sun ACC builtin int type.  */
		dtype = parse_stab_sun_builtin_type(dhandle, pp, p_end);
		break;

	case 'R':
		/* Sun ACC builtin float type.  */
		dtype = parse_stab_sun_floating_type(dhandle, pp, p_end);
		break;

	case 'e':
		/* Enumeration type.  */
		dtype = parse_stab_enum_type(dhandle, pp, p_end);
		break;

	case 's':
	case 'u':
		/* Struct or union type.  */
		dtype = parse_stab_struct_type(dhandle, info, type_name, pp,
			descriptor == 's', typenums, p_end);
		break;

	case 'a':
		/* Array type.  */
		if (**pp != 'r')
		{
			bad_stab(orig);
			return DEBUG_TYPE_NULL;
		}
		++* pp;

		dtype = parse_stab_array_type(dhandle, info, pp, stringp, p_end);
		break;

	case 'S':
		dtype = debug_make_set_type(dhandle,
			parse_stab_type(dhandle, info,
				(const char*)NULL,
				pp,
				(debug_type**)NULL,
				p_end),
			stringp);
		break;

	default:
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}

	if (dtype == DEBUG_TYPE_NULL)
		return DEBUG_TYPE_NULL;

	if (typenums[0] != -1)
	{
		if (!stab_record_type(dhandle, info, typenums, dtype))
			return DEBUG_TYPE_NULL;
	}

	if (size != -1)
	{
		if (!debug_record_type_size(dhandle, dtype, (unsigned int)size))
			return DEBUG_TYPE_NULL;
	}

	return dtype;
}

/* Read a number by which a type is referred to in dbx data, or
   perhaps read a pair (FILENUM, TYPENUM) in parentheses.  Just a
   single number N is equivalent to (0,N).  Return the two numbers by
   storing them in the vector TYPENUMS.  */

static bool
parse_stab_type_number(const char** pp, int* typenums, const char* p_end)
{
	const char* orig;

	orig = *pp;

	if (**pp != '(')
	{
		typenums[0] = 0;
		typenums[1] = (int)parse_number(pp, (bool*)NULL, p_end);
		return true;
	}

	++* pp;
	typenums[0] = (int)parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ',')
	{
		bad_stab(orig);
		return false;
	}

	++* pp;
	typenums[1] = (int)parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ')')
	{
		bad_stab(orig);
		return false;
	}

	++* pp;
	return true;
}

/* Parse a range type.  */

static debug_type
parse_stab_range_type(void* dhandle,
	struct stab_handle* info,
	const char* type_name,
	const char** pp,
	const int* typenums,
	const char* p_end)
{
	const char* orig;
	int rangenums[2];
	bool self_subrange;
	debug_type index_type;
	const char* s2, * s3;
	bfd_signed_vma n2, n3;
	bool ov2, ov3;

	orig = *pp;
	if (orig >= p_end)
		return DEBUG_TYPE_NULL;

	index_type = DEBUG_TYPE_NULL;

	/* First comes a type we are a subrange of.
	   In C it is usually 0, 1 or the type being defined.  */
	if (!parse_stab_type_number(pp, rangenums, p_end))
		return DEBUG_TYPE_NULL;

	self_subrange = (rangenums[0] == typenums[0]
		&& rangenums[1] == typenums[1]);

	if (**pp == '=')
	{
		*pp = orig;
		index_type = parse_stab_type(dhandle, info, (const char*)NULL,
			pp, (debug_type**)NULL, p_end);
		if (index_type == DEBUG_TYPE_NULL)
			return DEBUG_TYPE_NULL;
	}

	if (**pp == ';')
		++* pp;

	/* The remaining two operands are usually lower and upper bounds of
	   the range.  But in some special cases they mean something else.  */
	s2 = *pp;
	n2 = parse_number(pp, &ov2, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	s3 = *pp;
	n3 = parse_number(pp, &ov3, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	if (ov2 || ov3)
	{
		/* gcc will emit range stabs for long long types.  Handle this
		   as a special case.  FIXME: This needs to be more general.  */
#define LLLOW   "01000000000000000000000;"
#define LLHIGH   "0777777777777777777777;"
#define ULLHIGH "01777777777777777777777;"
		if (index_type == DEBUG_TYPE_NULL)
		{
			if (startswith(s2, LLLOW)
				&& startswith(s3, LLHIGH))
				return debug_make_int_type(dhandle, 8, false);
			if (!ov2
				&& n2 == 0
				&& startswith(s3, ULLHIGH))
				return debug_make_int_type(dhandle, 8, true);
		}

		warn_stab(orig, _("numeric overflow"));
	}

	if (index_type == DEBUG_TYPE_NULL)
	{
		/* A type defined as a subrange of itself, with both bounds 0,
		   is void.  */
		if (self_subrange && n2 == 0 && n3 == 0)
			return debug_make_void_type(dhandle);

		/* A type defined as a subrange of itself, with n2 positive and
	   n3 zero, is a complex type, and n2 is the number of bytes.  */
		if (self_subrange && n3 == 0 && n2 > 0)
			return debug_make_complex_type(dhandle, n2);

		/* If n3 is zero and n2 is positive, this is a floating point
		   type, and n2 is the number of bytes.  */
		if (n3 == 0 && n2 > 0)
			return debug_make_float_type(dhandle, n2);

		/* If the upper bound is -1, this is an unsigned int.  */
		if (n2 == 0 && n3 == -1)
		{
			/* When gcc is used with -gstabs, but not -gstabs+, it will emit
				   long long int:t6=r1;0;-1;
			   long long unsigned int:t7=r1;0;-1;
			   We hack here to handle this reasonably.  */
			if (type_name != NULL)
			{
				if (strcmp(type_name, "long long int") == 0)
					return debug_make_int_type(dhandle, 8, false);
				else if (strcmp(type_name, "long long unsigned int") == 0)
					return debug_make_int_type(dhandle, 8, true);
			}
			/* FIXME: The size here really depends upon the target.  */
			return debug_make_int_type(dhandle, 4, true);
		}

		/* A range of 0 to 127 is char.  */
		if (self_subrange && n2 == 0 && n3 == 127)
			return debug_make_int_type(dhandle, 1, false);

		/* FIXME: gdb checks for the language CHILL here.  */

		if (n2 == 0)
		{
			if (n3 < 0)
				return debug_make_int_type(dhandle, -n3, true);
			else if (n3 == 0xff)
				return debug_make_int_type(dhandle, 1, true);
			else if (n3 == 0xffff)
				return debug_make_int_type(dhandle, 2, true);
			else if (n3 == (bfd_signed_vma)0xffffffff)
				return debug_make_int_type(dhandle, 4, true);
#ifdef BFD64
			else if (n3 == (bfd_signed_vma)0xffffffffffffffffLL)
				return debug_make_int_type(dhandle, 8, true);
#endif
		}
		else if (n3 == 0
			&& n2 < 0
			&& (self_subrange || n2 == -8))
			return debug_make_int_type(dhandle, -n2, true);
		else if (n2 == -n3 - 1 || n2 == n3 + 1)
		{
			if (n3 == 0x7f)
				return debug_make_int_type(dhandle, 1, false);
			else if (n3 == 0x7fff)
				return debug_make_int_type(dhandle, 2, false);
			else if (n3 == 0x7fffffff)
				return debug_make_int_type(dhandle, 4, false);
#ifdef BFD64
			else if (n3 == ((((bfd_vma)0x7fffffff) << 32) | 0xffffffff))
				return debug_make_int_type(dhandle, 8, false);
#endif
		}
	}

	/* At this point I don't have the faintest idea how to deal with a
	   self_subrange type; I'm going to assume that this is used as an
	   idiom, and that all of them are special cases.  So . . .  */
	if (self_subrange)
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}

	index_type = stab_find_type(dhandle, info, rangenums);
	if (index_type == DEBUG_TYPE_NULL)
	{
		/* Does this actually ever happen?  Is that why we are worrying
		   about dealing with it rather than just calling error_type?  */
		warn_stab(orig, _("missing index type"));
		index_type = debug_make_int_type(dhandle, 4, false);
	}

	return debug_make_range_type(dhandle, index_type, n2, n3);
}

/* Sun's ACC uses a somewhat saner method for specifying the builtin
   typedefs in every file (for int, long long, etc):

	type = b <signed> <width>; <offset>; <nbits>
	signed = u or s.  Possible c in addition to u or s (for char?).
	offset = offset from high order bit to start bit of type.
	width is # bytes in object of this type, nbits is # bits in type.

   The width/offset stuff appears to be for small objects stored in
   larger ones (e.g. `shorts' in `int' registers).  We ignore it for now,
   FIXME.  */

static debug_type
parse_stab_sun_builtin_type(void* dhandle, const char** pp, const char* p_end)
{
	const char* orig;
	bool unsignedp;
	bfd_vma bits;

	orig = *pp;
	if (orig >= p_end)
		return DEBUG_TYPE_NULL;

	switch (**pp)
	{
	case 's':
		unsignedp = false;
		break;
	case 'u':
		unsignedp = true;
		break;
	default:
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	/* OpenSolaris source code indicates that one of "cbv" characters
	   can come next and specify the intrinsic 'iformat' encoding.
	   'c' is character encoding, 'b' is boolean encoding, and 'v' is
	   varargs encoding.  This field can be safely ignored because
	   the type of the field is determined from the bitwidth extracted
	   below.  */
	if (**pp == 'c' || **pp == 'b' || **pp == 'v')
		++* pp;

	/* The first number appears to be the number of bytes occupied
	   by this type, except that unsigned short is 4 instead of 2.
	   Since this information is redundant with the third number,
	   we will ignore it.  */
	(void)parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	/* The second number is always 0, so ignore it too.  */
	(void)parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	/* The third number is the number of bits for this type.  */
	bits = parse_number(pp, (bool*)NULL, p_end);

	/* The type *should* end with a semicolon.  If it are embedded
	   in a larger type the semicolon may be the only way to know where
	   the type ends.  If this type is at the end of the stabstring we
	   can deal with the omitted semicolon (but we don't have to like
	   it).  Don't bother to complain(), Sun's compiler omits the semicolon
	   for "void".  */
	if (**pp == ';')
		++* pp;

	if (bits == 0)
		return debug_make_void_type(dhandle);

	return debug_make_int_type(dhandle, bits / 8, unsignedp);
}

/* Parse a builtin floating type generated by the Sun compiler.  */

static debug_type
parse_stab_sun_floating_type(void* dhandle, const char** pp, const char* p_end)
{
	const char* orig;
	bfd_vma details;
	bfd_vma bytes;

	orig = *pp;
	if (orig >= p_end)
		return DEBUG_TYPE_NULL;

	/* The first number has more details about the type, for example
	   FN_COMPLEX.  */
	details = parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}

	/* The second number is the number of bytes occupied by this type */
	bytes = parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}

	if (details == NF_COMPLEX
		|| details == NF_COMPLEX16
		|| details == NF_COMPLEX32)
		return debug_make_complex_type(dhandle, bytes);

	return debug_make_float_type(dhandle, bytes);
}

/* Handle an enum type.  */

static debug_type
parse_stab_enum_type(void* dhandle, const char** pp, const char* p_end)
{
	const char* orig;
	const char** names, ** xnames;
	bfd_signed_vma* values, * xvalues;
	unsigned int n;
	unsigned int alloc;

	orig = *pp;
	if (orig >= p_end)
		return DEBUG_TYPE_NULL;

	/* FIXME: gdb checks os9k_stabs here.  */

	/* The aix4 compiler emits an extra field before the enum members;
	   my guess is it's a type of some sort.  Just ignore it.  */
	if (**pp == '-')
	{
		while (**pp != ':' && **pp != 0)
			++* pp;

		if (**pp == 0)
		{
			bad_stab(orig);
			return DEBUG_TYPE_NULL;
		}
		++* pp;
	}

	/* Read the value-names and their values.
	   The input syntax is NAME:VALUE,NAME:VALUE, and so on.
	   A semicolon or comma instead of a NAME means the end.  */
	alloc = 10;
	names = xmalloc(alloc * sizeof(*names));
	values = xmalloc(alloc * sizeof(*values));
	n = 0;
	while (**pp != '\0' && **pp != ';' && **pp != ',')
	{
		const char* p;
		char* name;
		bfd_signed_vma val;

		p = *pp;
		while (*p != ':' && *p != 0)
			++p;

		if (*p == 0)
		{
			bad_stab(orig);
			free(names);
			free(values);
			return DEBUG_TYPE_NULL;
		}

		name = savestring(dhandle, *pp, p - *pp);

		*pp = p + 1;
		val = (bfd_signed_vma)parse_number(pp, (bool*)NULL, p_end);
		if (**pp != ',')
		{
			bad_stab(orig);
			free(names);
			free(values);
			return DEBUG_TYPE_NULL;
		}
		++* pp;

		if (n + 1 >= alloc)
		{
			alloc += 10;
			names = xrealloc(names, alloc * sizeof(*names));
			values = xrealloc(values, alloc * sizeof(*values));
		}

		names[n] = name;
		values[n] = val;
		++n;
	}

	names[n] = NULL;
	values[n] = 0;
	xnames = debug_xalloc(dhandle, (n + 1) * sizeof(*names));
	memcpy(xnames, names, (n + 1) * sizeof(*names));
	free(names);
	xvalues = debug_xalloc(dhandle, (n + 1) * sizeof(*names));
	memcpy(xvalues, values, (n + 1) * sizeof(*names));
	free(values);

	if (**pp == ';')
		++* pp;

	return debug_make_enum_type(dhandle, xnames, xvalues);
}

/* Read the description of a structure (or union type) and return an object
   describing the type.

   PP points to a character pointer that points to the next unconsumed token
   in the stabs string.  For example, given stabs "A:T4=s4a:1,0,32;;",
   *PP will point to "4a:1,0,32;;".  */

static debug_type
parse_stab_struct_type(void* dhandle,
	struct stab_handle* info,
	const char* tagname,
	const char** pp,
	bool structp,
	const int* typenums,
	const char* p_end)
{
	bfd_vma size;
	debug_baseclass* baseclasses;
	debug_field* fields = NULL;
	bool statics;
	debug_method* methods;
	debug_type vptrbase;
	bool ownvptr;

	/* Get the size.  */
	size = parse_number(pp, (bool*)NULL, p_end);

	/* Get the other information.  */
	if (!parse_stab_baseclasses(dhandle, info, pp, &baseclasses, p_end)
		|| !parse_stab_struct_fields(dhandle, info, pp, &fields, &statics, p_end)
		|| !parse_stab_members(dhandle, info, tagname, pp, typenums, &methods, p_end)
		|| !parse_stab_tilde_field(dhandle, info, pp, typenums, &vptrbase,
			&ownvptr, p_end))
		return DEBUG_TYPE_NULL;

	if (!statics
		&& baseclasses == NULL
		&& methods == NULL
		&& vptrbase == DEBUG_TYPE_NULL
		&& !ownvptr)
		return debug_make_struct_type(dhandle, structp, size, fields);

	return debug_make_object_type(dhandle, structp, size, fields, baseclasses,
		methods, vptrbase, ownvptr);
}

/* The stabs for C++ derived classes contain baseclass information which
   is marked by a '!' character after the total size.  This function is
   called when we encounter the baseclass marker, and slurps up all the
   baseclass information.

   Immediately following the '!' marker is the number of base classes that
   the class is derived from, followed by information for each base class.
   For each base class, there are two visibility specifiers, a bit offset
   to the base class information within the derived class, a reference to
   the type for the base class, and a terminating semicolon.

   A typical example, with two base classes, would be "!2,020,19;0264,21;".
							   ^^ ^ ^ ^  ^ ^  ^
	Baseclass information marker __________________|| | | |  | |  |
	Number of baseclasses __________________________| | | |  | |  |
	Visibility specifiers (2) ________________________| | |  | |  |
	Offset in bits from start of class _________________| |  | |  |
	Type number for base class ___________________________|  | |  |
	Visibility specifiers (2) _______________________________| |  |
	Offset in bits from start of class ________________________|  |
	Type number of base class ____________________________________|

  Return TRUE for success, FALSE for failure.  */

static bool
parse_stab_baseclasses(void* dhandle,
	struct stab_handle* info,
	const char** pp,
	debug_baseclass** retp,
	const char* p_end)
{
	const char* orig;
	unsigned int c, i;
	debug_baseclass* classes;

	*retp = NULL;

	orig = *pp;
	if (orig >= p_end)
		return false;

	if (**pp != '!')
	{
		/* No base classes.  */
		return true;
	}
	++* pp;

	c = (unsigned int)parse_number(pp, (bool*)NULL, p_end);

	if (**pp != ',')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	classes = debug_xalloc(dhandle, (c + 1) * sizeof(*classes));

	for (i = 0; i < c; i++)
	{
		bool is_virtual;
		enum debug_visibility visibility;
		bfd_vma bitpos;
		debug_type type;

		switch (**pp)
		{
		case '0':
			is_virtual = false;
			break;
		case '1':
			is_virtual = true;
			break;
		case 0:
			bad_stab(orig);
			return false;
		default:
			warn_stab(orig, _("unknown virtual character for baseclass"));
			is_virtual = false;
			break;
		}
		++* pp;

		switch (**pp)
		{
		case '0':
			visibility = DEBUG_VISIBILITY_PRIVATE;
			break;
		case '1':
			visibility = DEBUG_VISIBILITY_PROTECTED;
			break;
		case '2':
			visibility = DEBUG_VISIBILITY_PUBLIC;
			break;
		case 0:
			bad_stab(orig);
			return false;
		default:
			warn_stab(orig, _("unknown visibility character for baseclass"));
			visibility = DEBUG_VISIBILITY_PUBLIC;
			break;
		}
		++* pp;

		/* The remaining value is the bit offset of the portion of the
	   object corresponding to this baseclass.  Always zero in the
	   absence of multiple inheritance.  */
		bitpos = parse_number(pp, (bool*)NULL, p_end);
		if (**pp != ',')
		{
			bad_stab(orig);
			return false;
		}
		++* pp;

		type = parse_stab_type(dhandle, info, (const char*)NULL, pp,
			(debug_type**)NULL, p_end);
		if (type == DEBUG_TYPE_NULL)
			return false;

		classes[i] = debug_make_baseclass(dhandle, type, bitpos, is_virtual,
			visibility);
		if (classes[i] == DEBUG_BASECLASS_NULL)
			return false;

		if (**pp != ';')
			return false;
		++* pp;
	}

	classes[i] = DEBUG_BASECLASS_NULL;

	*retp = classes;

	return true;
}

/* Read struct or class data fields.  They have the form:

	NAME : [VISIBILITY] TYPENUM , BITPOS , BITSIZE ;

   At the end, we see a semicolon instead of a field.

   In C++, this may wind up being NAME:?TYPENUM:PHYSNAME; for
   a static field.

   The optional VISIBILITY is one of:

	'/0'	(VISIBILITY_PRIVATE)
	'/1'	(VISIBILITY_PROTECTED)
	'/2'	(VISIBILITY_PUBLIC)
	'/9'	(VISIBILITY_IGNORE)

   or nothing, for C style fields with public visibility.

   Returns 1 for success, 0 for failure.  */

static bool
parse_stab_struct_fields(void* dhandle,
	struct stab_handle* info,
	const char** pp,
	debug_field** retp,
	bool* staticsp,
	const char* p_end)
{
	const char* orig;
	const char* p;
	debug_field* fields, * xfields;
	unsigned int c;
	unsigned int alloc;

	*retp = NULL;
	*staticsp = false;

	orig = *pp;
	if (orig >= p_end)
		return false;

	c = 0;
	alloc = 10;
	fields = xmalloc(alloc * sizeof(*fields));
	while (**pp != ';')
	{
		/* FIXME: gdb checks os9k_stabs here.  */

		p = *pp;

		/* Add 1 to c to leave room for NULL pointer at end.  */
		if (c + 1 >= alloc)
		{
			alloc += 10;
			fields = xrealloc(fields, alloc * sizeof(*fields));
		}

		/* If it starts with CPLUS_MARKER it is a special abbreviation,
	   unless the CPLUS_MARKER is followed by an underscore, in
	   which case it is just the name of an anonymous type, which we
	   should handle like any other type name.  We accept either '$'
	   or '.', because a field name can never contain one of these
	   characters except as a CPLUS_MARKER.  */

		if ((*p == '$' || *p == '.') && p[1] != '_')
		{
			++* pp;
			if (!parse_stab_cpp_abbrev(dhandle, info, pp, fields + c, p_end))
			{
				free(fields);
				return false;
			}
			++c;
			continue;
		}

		/* Look for the ':' that separates the field name from the field
	   values.  Data members are delimited by a single ':', while member
	   functions are delimited by a pair of ':'s.  When we hit the member
	   functions (if any), terminate scan loop and return.  */

		p = strchr(p, ':');
		if (p == NULL)
		{
			bad_stab(orig);
			free(fields);
			return false;
		}

		if (p[1] == ':')
			break;

		if (!parse_stab_one_struct_field(dhandle, info, pp, p, fields + c,
			staticsp, p_end))
		{
			free(fields);
			return false;
		}

		++c;
	}

	fields[c] = DEBUG_FIELD_NULL;
	xfields = debug_xalloc(dhandle, (c + 1) * sizeof(*fields));
	memcpy(xfields, fields, (c + 1) * sizeof(*fields));
	free(fields);

	*retp = xfields;

	return true;
}

/* Special GNU C++ name.  */

static bool
parse_stab_cpp_abbrev(void* dhandle,
	struct stab_handle* info,
	const char** pp,
	debug_field* retp,
	const char* p_end)
{
	const char* orig;
	int cpp_abbrev;
	debug_type context;
	const char* name;
	const char* type_name;
	debug_type type;
	bfd_vma bitpos;
	size_t len;

	*retp = DEBUG_FIELD_NULL;

	orig = *pp;
	if (orig >= p_end)
		return false;

	if (**pp != 'v')
	{
		bad_stab(*pp);
		return false;
	}
	++* pp;

	cpp_abbrev = **pp;
	if (cpp_abbrev == 0)
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	/* At this point, *pp points to something like "22:23=*22...", where
	   the type number before the ':' is the "context" and everything
	   after is a regular type definition.  Lookup the type, find it's
	   name, and construct the field name.  */

	context = parse_stab_type(dhandle, info, (const char*)NULL, pp,
		(debug_type**)NULL, p_end);
	if (context == DEBUG_TYPE_NULL)
		return false;

	switch (cpp_abbrev)
	{
	case 'f':
		/* $vf -- a virtual function table pointer.  */
		name = "_vptr$";
		break;
	case 'b':
		/* $vb -- a virtual bsomethingorother */
		type_name = debug_get_type_name(dhandle, context);
		if (type_name == NULL)
		{
			warn_stab(orig, _("unnamed $vb type"));
			type_name = "FOO";
		}
		len = strlen(type_name);
		name = debug_xalloc(dhandle, len + sizeof("_vb$"));
		memcpy((char*)name, "_vb$", sizeof("_vb$") - 1);
		memcpy((char*)name + sizeof("_vb$") - 1, type_name, len + 1);
		break;
	default:
		warn_stab(orig, _("unrecognized C++ abbreviation"));
		name = "INVALID_CPLUSPLUS_ABBREV";
		break;
	}

	if (**pp != ':')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	type = parse_stab_type(dhandle, info, (const char*)NULL, pp,
		(debug_type**)NULL, p_end);
	if (**pp != ',')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	bitpos = parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	*retp = debug_make_field(dhandle, name, type, bitpos, 0,
		DEBUG_VISIBILITY_PRIVATE);
	if (*retp == DEBUG_FIELD_NULL)
		return false;

	return true;
}

/* Parse a single field in a struct or union.  */

static bool
parse_stab_one_struct_field(void* dhandle,
	struct stab_handle* info,
	const char** pp,
	const char* p,
	debug_field* retp,
	bool* staticsp,
	const char* p_end)
{
	const char* orig;
	char* name;
	enum debug_visibility visibility;
	debug_type type;
	bfd_vma bitpos;
	bfd_vma bitsize;

	orig = *pp;
	if (orig >= p_end)
		return false;

	/* FIXME: gdb checks ARM_DEMANGLING here.  */

	name = savestring(dhandle, *pp, p - *pp);

	*pp = p + 1;

	if (**pp != '/')
		visibility = DEBUG_VISIBILITY_PUBLIC;
	else
	{
		++* pp;
		switch (**pp)
		{
		case '0':
			visibility = DEBUG_VISIBILITY_PRIVATE;
			break;
		case '1':
			visibility = DEBUG_VISIBILITY_PROTECTED;
			break;
		case '2':
			visibility = DEBUG_VISIBILITY_PUBLIC;
			break;
		case 0:
			bad_stab(orig);
			return false;
		default:
			warn_stab(orig, _("unknown visibility character for field"));
			visibility = DEBUG_VISIBILITY_PUBLIC;
			break;
		}
		++* pp;
	}

	type = parse_stab_type(dhandle, info, (const char*)NULL, pp,
		(debug_type**)NULL, p_end);
	if (type == DEBUG_TYPE_NULL)
		return false;

	if (**pp == ':')
	{
		char* varname;

		/* This is a static class member.  */
		++* pp;
		p = strchr(*pp, ';');
		if (p == NULL)
		{
			bad_stab(orig);
			return false;
		}

		varname = savestring(dhandle, *pp, p - *pp);

		*pp = p + 1;

		*retp = debug_make_static_member(dhandle, name, type, varname,
			visibility);
		*staticsp = true;

		return true;
	}

	if (**pp != ',')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	bitpos = parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ',')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	bitsize = parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return false;
	}
	++* pp;

	if (bitpos == 0 && bitsize == 0)
	{
		/* This can happen in two cases: (1) at least for gcc 2.4.5 or
	   so, it is a field which has been optimized out.  The correct
	   stab for this case is to use VISIBILITY_IGNORE, but that is a
	   recent invention.  (2) It is a 0-size array.  For example
	   union { int num; char str[0]; } foo.  Printing "<no value>"
	   for str in "p foo" is OK, since foo.str (and thus foo.str[3])
	   will continue to work, and a 0-size array as a whole doesn't
	   have any contents to print.

	   I suspect this probably could also happen with gcc -gstabs
	   (not -gstabs+) for static fields, and perhaps other C++
	   extensions.  Hopefully few people use -gstabs with gdb, since
	   it is intended for dbx compatibility.  */
		visibility = DEBUG_VISIBILITY_IGNORE;
	}

	/* FIXME: gdb does some stuff here to mark fields as unpacked.  */

	*retp = debug_make_field(dhandle, name, type, bitpos, bitsize, visibility);

	return true;
}

/* Read member function stabs info for C++ classes.  The form of each member
   function data is:

	NAME :: TYPENUM[=type definition] ARGS : PHYSNAME ;

   An example with two member functions is:

	afunc1::20=##15;:i;2A.;afunc2::20:i;2A.;

   For the case of overloaded operators, the format is op$::*.funcs, where
   $ is the CPLUS_MARKER (usually '$'), `*' holds the place for an operator
   name (such as `+=') and `.' marks the end of the operator name.  */

static bool
parse_stab_members(void* dhandle,
	struct stab_handle* info,
	const char* tagname,
	const char** pp,
	const int* typenums,
	debug_method** retp,
	const char* p_end)
{
	const char* orig;
	debug_method* methods, * xmethods;
	unsigned int c;
	unsigned int alloc;
	char* name = NULL;
	debug_method_variant* variants = NULL, * xvariants;
	char* argtypes = NULL;

	*retp = NULL;

	orig = *pp;
	if (orig >= p_end)
		return false;

	alloc = 0;
	methods = NULL;
	c = 0;

	while (**pp != ';')
	{
		const char* p;
		unsigned int cvars;
		unsigned int allocvars;
		debug_type look_ahead_type;

		p = strchr(*pp, ':');
		if (p == NULL || p[1] != ':')
			break;

		/* FIXME: Some systems use something other than '$' here.  */
		if ((*pp)[0] != 'o' || (*pp)[1] != 'p' || (*pp)[2] != '$')
		{
			name = savestring(dhandle, *pp, p - *pp);
			*pp = p + 2;
		}
		else
		{
			/* This is a completely weird case.  In order to stuff in the
			   names that might contain colons (the usual name delimiter),
			   Mike Tiemann defined a different name format which is
			   signalled if the identifier is "op$".  In that case, the
			   format is "op$::XXXX." where XXXX is the name.  This is
			   used for names like "+" or "=".  YUUUUUUUK!  FIXME!  */
			*pp = p + 2;
			for (p = *pp; *p != '.' && *p != '\0'; p++)
				;
			if (*p != '.')
			{
				bad_stab(orig);
				goto fail;
			}
			name = savestring(dhandle, *pp, p - *pp);
			*pp = p + 1;
		}

		allocvars = 10;
		variants = xmalloc(allocvars * sizeof(*variants));
		cvars = 0;

		look_ahead_type = DEBUG_TYPE_NULL;

		do
		{
			debug_type type;
			bool stub;
			enum debug_visibility visibility;
			bool constp, volatilep, staticp;
			bfd_vma voffset;
			debug_type context;
			const char* physname;
			bool varargs;

			if (look_ahead_type != DEBUG_TYPE_NULL)
			{
				/* g++ version 1 kludge */
				type = look_ahead_type;
				look_ahead_type = DEBUG_TYPE_NULL;
			}
			else
			{
				type = parse_stab_type(dhandle, info, (const char*)NULL, pp,
					(debug_type**)NULL, p_end);
				if (type == DEBUG_TYPE_NULL)
					goto fail;

				if (**pp != ':')
				{
					bad_stab(orig);
					goto fail;
				}
			}

			++* pp;
			p = strchr(*pp, ';');
			if (p == NULL)
			{
				bad_stab(orig);
				goto fail;
			}

			stub = false;
			if (debug_get_type_kind(dhandle, type) == DEBUG_KIND_METHOD
				&& debug_get_parameter_types(dhandle, type, &varargs) == NULL)
				stub = true;

			argtypes = savestring(dhandle, *pp, p - *pp);
			*pp = p + 1;

			switch (**pp)
			{
			case '0':
				visibility = DEBUG_VISIBILITY_PRIVATE;
				break;
			case '1':
				visibility = DEBUG_VISIBILITY_PROTECTED;
				break;
			case 0:
				bad_stab(orig);
				goto fail;
			default:
				visibility = DEBUG_VISIBILITY_PUBLIC;
				break;
			}
			++* pp;

			constp = false;
			volatilep = false;
			switch (**pp)
			{
			case 'A':
				/* Normal function.  */
				++ * pp;
				break;
			case 'B':
				/* const member function.  */
				constp = true;
				++* pp;
				break;
			case 'C':
				/* volatile member function.  */
				volatilep = true;
				++* pp;
				break;
			case 'D':
				/* const volatile member function.  */
				constp = true;
				volatilep = true;
				++* pp;
				break;
			case '*':
			case '?':
			case '.':
				/* File compiled with g++ version 1; no information.  */
				break;
			default:
				warn_stab(orig, _("const/volatile indicator missing"));
				break;
			}

			staticp = false;
			switch (**pp)
			{
			case '*':
				/* virtual member function, followed by index.  The sign
			   bit is supposedly set to distinguish
			   pointers-to-methods from virtual function indices.  */
				++ * pp;
				voffset = parse_number(pp, (bool*)NULL, p_end);
				if (**pp != ';')
				{
					bad_stab(orig);
					goto fail;
				}
				++* pp;
				voffset &= 0x7fffffff;

				if (**pp == ';' || **pp == '\0')
				{
					/* Must be g++ version 1.  */
					context = DEBUG_TYPE_NULL;
				}
				else
				{
					/* Figure out from whence this virtual function
					   came.  It may belong to virtual function table of
					   one of its baseclasses.  */
					look_ahead_type = parse_stab_type(dhandle, info,
						(const char*)NULL,
						pp,
						(debug_type**)NULL,
						p_end);
					if (**pp == ':')
					{
						/* g++ version 1 overloaded methods.  */
						context = DEBUG_TYPE_NULL;
					}
					else
					{
						context = look_ahead_type;
						look_ahead_type = DEBUG_TYPE_NULL;
						if (**pp != ';')
						{
							bad_stab(orig);
							goto fail;
						}
						++* pp;
					}
				}
				break;

			case '?':
				/* static member function.  */
				++ * pp;
				staticp = true;
				voffset = 0;
				context = DEBUG_TYPE_NULL;
				if (strncmp(argtypes, name, strlen(name)) != 0)
					stub = true;
				break;

			default:
				warn_stab(orig, "member function type missing");
				voffset = 0;
				context = DEBUG_TYPE_NULL;
				break;

			case '.':
				++ * pp;
				voffset = 0;
				context = DEBUG_TYPE_NULL;
				break;
			}

			/* If the type is not a stub, then the argtypes string is
				   the physical name of the function.  Otherwise the
				   argtypes string is the mangled form of the argument
				   types, and the full type and the physical name must be
				   extracted from them.  */
			physname = argtypes;
			if (stub)
			{
				debug_type class_type, return_type;

				class_type = stab_find_type(dhandle, info, typenums);
				if (class_type == DEBUG_TYPE_NULL)
					goto fail;
				return_type = debug_get_return_type(dhandle, type);
				if (return_type == DEBUG_TYPE_NULL)
				{
					bad_stab(orig);
					goto fail;
				}
				type = parse_stab_argtypes(dhandle, info, class_type, name,
					tagname, return_type, argtypes,
					constp, volatilep, &physname);
				if (type == DEBUG_TYPE_NULL)
					goto fail;
			}

			if (cvars + 1 >= allocvars)
			{
				allocvars += 10;
				variants = xrealloc(variants, allocvars * sizeof(*variants));
			}

			if (!staticp)
				variants[cvars] = debug_make_method_variant(dhandle, physname,
					type, visibility,
					constp, volatilep,
					voffset, context);
			else
				variants[cvars] = debug_make_static_method_variant(dhandle,
					physname,
					type,
					visibility,
					constp,
					volatilep);
			if (variants[cvars] == DEBUG_METHOD_VARIANT_NULL)
				goto fail;

			++cvars;
		} while (**pp != ';' && **pp != '\0');

		variants[cvars] = DEBUG_METHOD_VARIANT_NULL;
		xvariants = debug_xalloc(dhandle, (cvars + 1) * sizeof(*variants));
		memcpy(xvariants, variants, (cvars + 1) * sizeof(*variants));
		free(variants);

		if (**pp != '\0')
			++* pp;

		if (c + 1 >= alloc)
		{
			alloc += 10;
			methods = xrealloc(methods, alloc * sizeof(*methods));
		}

		methods[c] = debug_make_method(dhandle, name, xvariants);

		++c;
	}

	xmethods = methods;
	if (methods != NULL)
	{
		methods[c] = DEBUG_METHOD_NULL;
		xmethods = debug_xalloc(dhandle, (c + 1) * sizeof(*methods));
		memcpy(xmethods, methods, (c + 1) * sizeof(*methods));
		free(methods);
	}

	*retp = xmethods;

	return true;

fail:
	free(variants);
	free(methods);
	return false;
}

/* Parse a string representing argument types for a method.  Stabs
   tries to save space by packing argument types into a mangled
   string.  This string should give us enough information to extract
   both argument types and the physical name of the function, given
   the tag name.  */

static debug_type
parse_stab_argtypes(void* dhandle, struct stab_handle* info,
	debug_type class_type, const char* fieldname,
	const char* tagname, debug_type return_type,
	const char* argtypes, bool constp,
	bool volatilep, const char** pphysname)
{
	bool is_full_physname_constructor;
	bool is_constructor;
	bool is_destructor;
	bool is_v3;
	debug_type* args;
	bool varargs;
	unsigned int physname_len = 0;

	/* Constructors are sometimes handled specially.  */
	is_full_physname_constructor = ((argtypes[0] == '_'
		&& argtypes[1] == '_'
		&& (ISDIGIT(argtypes[2])
			|| argtypes[2] == 'Q'
			|| argtypes[2] == 't'))
		|| startswith(argtypes, "__ct"));

	is_constructor = (is_full_physname_constructor
		|| (tagname != NULL
			&& strcmp(fieldname, tagname) == 0));
	is_destructor = ((argtypes[0] == '_'
		&& (argtypes[1] == '$' || argtypes[1] == '.')
		&& argtypes[2] == '_')
		|| startswith(argtypes, "__dt"));
	is_v3 = argtypes[0] == '_' && argtypes[1] == 'Z';

	if (!(is_destructor || is_full_physname_constructor || is_v3))
	{
		unsigned int len;
		const char* const_prefix;
		const char* volatile_prefix;
		char buf[20];
		unsigned int mangled_name_len;
		char* physname;

		len = tagname == NULL ? 0 : strlen(tagname);
		const_prefix = constp ? "C" : "";
		volatile_prefix = volatilep ? "V" : "";

		if (len == 0)
			sprintf(buf, "__%s%s", const_prefix, volatile_prefix);
		else if (tagname != NULL && strchr(tagname, '<') != NULL)
		{
			/* Template methods are fully mangled.  */
			sprintf(buf, "__%s%s", const_prefix, volatile_prefix);
			tagname = NULL;
			len = 0;
		}
		else
			sprintf(buf, "__%s%s%d", const_prefix, volatile_prefix, len);

		mangled_name_len = ((is_constructor ? 0 : strlen(fieldname))
			+ strlen(buf)
			+ len
			+ strlen(argtypes)
			+ 1);

		if (fieldname[0] == 'o'
			&& fieldname[1] == 'p'
			&& (fieldname[2] == '$' || fieldname[2] == '.'))
		{
			/* Opname selection is no longer supported by libiberty's demangler.  */
			return DEBUG_TYPE_NULL;
		}

		physname = debug_xalloc(dhandle, mangled_name_len);
		if (is_constructor)
			physname[0] = '\0';
		else
			strcpy(physname, fieldname);

		physname_len = strlen(physname);
		strcat(physname, buf);
		if (tagname != NULL)
			strcat(physname, tagname);
		strcat(physname, argtypes);

		*pphysname = physname;
	}

	if (*argtypes == '\0' || is_destructor)
	{
		args = debug_xalloc(dhandle, sizeof(*args));
		*args = NULL;
		return debug_make_method_type(dhandle, return_type, class_type, args,
			false);
	}

	args = stab_demangle_argtypes(dhandle, info, *pphysname, &varargs, physname_len);
	if (args == NULL)
		return DEBUG_TYPE_NULL;

	return debug_make_method_type(dhandle, return_type, class_type, args,
		varargs);
}

/* The tail end of stabs for C++ classes that contain a virtual function
   pointer contains a tilde, a %, and a type number.
   The type number refers to the base class (possibly this class itself) which
   contains the vtable pointer for the current class.

   This function is called when we have parsed all the method declarations,
   so we can look for the vptr base class info.  */

static bool
parse_stab_tilde_field(void* dhandle,
	struct stab_handle* info,
	const char** pp,
	const int* typenums,
	debug_type* retvptrbase,
	bool* retownvptr,
	const char* p_end)
{
	const char* orig;
	const char* hold;
	int vtypenums[2];

	*retvptrbase = DEBUG_TYPE_NULL;
	*retownvptr = false;

	orig = *pp;
	if (orig >= p_end)
		return false;

	/* If we are positioned at a ';', then skip it.  */
	if (**pp == ';')
		++* pp;

	if (**pp != '~')
		return true;
	++* pp;

	if (**pp == '=' || **pp == '+' || **pp == '-')
	{
		/* Obsolete flags that used to indicate the presence of
	   constructors and/or destructors.  */
		++* pp;
	}

	if (**pp != '%')
		return true;
	++* pp;

	hold = *pp;

	/* The next number is the type number of the base class (possibly
	   our own class) which supplies the vtable for this class.  */
	if (!parse_stab_type_number(pp, vtypenums, p_end))
		return false;

	if (vtypenums[0] == typenums[0]
		&& vtypenums[1] == typenums[1])
		*retownvptr = true;
	else
	{
		debug_type vtype;
		const char* p;

		*pp = hold;

		vtype = parse_stab_type(dhandle, info, (const char*)NULL, pp,
			(debug_type**)NULL, p_end);
		for (p = *pp; *p != ';' && *p != '\0'; p++)
			;
		if (*p != ';')
		{
			bad_stab(orig);
			return false;
		}

		*retvptrbase = vtype;

		*pp = p + 1;
	}

	return true;
}

/* Read a definition of an array type.  */

static debug_type
parse_stab_array_type(void* dhandle,
	struct stab_handle* info,
	const char** pp,
	bool stringp,
	const char* p_end)
{
	const char* orig;
	const char* p;
	int typenums[2];
	debug_type index_type;
	bool adjustable;
	bfd_signed_vma lower, upper;
	debug_type element_type;

	/* Format of an array type:
	   "ar<index type>;lower;upper;<array_contents_type>".
	   OS9000: "arlower,upper;<array_contents_type>".

	   Fortran adjustable arrays use Adigits or Tdigits for lower or upper;
	   for these, produce a type like float[][].  */

	orig = *pp;
	if (orig >= p_end)
		return DEBUG_TYPE_NULL;

	/* FIXME: gdb checks os9k_stabs here.  */

	/* If the index type is type 0, we take it as int.  */
	p = *pp;
	if (!parse_stab_type_number(&p, typenums, p_end))
		return DEBUG_TYPE_NULL;

	if (typenums[0] == 0 && typenums[1] == 0 && **pp != '=')
	{
		index_type = debug_find_named_type(dhandle, "int");
		if (index_type == DEBUG_TYPE_NULL)
		{
			index_type = debug_make_int_type(dhandle, 4, false);
			if (index_type == DEBUG_TYPE_NULL)
				return DEBUG_TYPE_NULL;
		}
		*pp = p;
	}
	else
	{
		index_type = parse_stab_type(dhandle, info, (const char*)NULL, pp,
			(debug_type**)NULL, p_end);
	}

	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	adjustable = false;

	if (!ISDIGIT(**pp) && **pp != '-' && **pp != 0)
	{
		++* pp;
		adjustable = true;
	}

	lower = (bfd_signed_vma)parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	if (!ISDIGIT(**pp) && **pp != '-' && **pp != 0)
	{
		++* pp;
		adjustable = true;
	}

	upper = (bfd_signed_vma)parse_number(pp, (bool*)NULL, p_end);
	if (**pp != ';')
	{
		bad_stab(orig);
		return DEBUG_TYPE_NULL;
	}
	++* pp;

	element_type = parse_stab_type(dhandle, info, (const char*)NULL, pp,
		(debug_type**)NULL, p_end);
	if (element_type == DEBUG_TYPE_NULL)
		return DEBUG_TYPE_NULL;

	if (adjustable)
	{
		lower = 0;
		upper = -1;
	}

	return debug_make_array_type(dhandle, element_type, index_type, lower,
		upper, stringp);
}

/* This struct holds information about files we have seen using
   N_BINCL.  */

struct bincl_file
{
	/* The next N_BINCL file.  */
	struct bincl_file* next;
	/* The next N_BINCL on the stack.  */
	struct bincl_file* next_stack;
	/* The file name.  */
	const char* name;
	/* The hash value.  */
	bfd_vma hash;
	/* The file index.  */
	unsigned int file;
	/* The list of types defined in this file.  */
	struct stab_types* file_types;
};

/* Start a new N_BINCL file, pushing it onto the stack.  */

static void
push_bincl(void* dhandle, struct stab_handle* info, const char* name,
	bfd_vma hash)
{
	struct bincl_file* n;

	n = debug_xalloc(dhandle, sizeof * n);
	n->next = info->bincl_list;
	n->next_stack = info->bincl_stack;
	n->name = name;
	n->hash = hash;
	n->file = info->files;
	n->file_types = NULL;
	info->bincl_list = n;
	info->bincl_stack = n;

	++info->files;
	info->file_types = xrealloc(info->file_types,
		info->files * sizeof(*info->file_types));
	info->file_types[n->file] = NULL;
}

/* Finish an N_BINCL file, at an N_EINCL, popping the name off the
   stack.  */

static const char*
pop_bincl(struct stab_handle* info)
{
	struct bincl_file* o;

	o = info->bincl_stack;
	if (o == NULL)
		return info->main_filename;
	info->bincl_stack = o->next_stack;

	if (o->file >= info->files)
		return info->main_filename;

	o->file_types = info->file_types[o->file];

	if (info->bincl_stack == NULL)
		return info->main_filename;
	return info->bincl_stack->name;
}

/* Handle an N_EXCL: get the types from the corresponding N_BINCL.  */

static bool
find_excl(struct stab_handle* info, const char* name, bfd_vma hash)
{
	struct bincl_file* l;

	++info->files;
	info->file_types = xrealloc(info->file_types,
		info->files * sizeof(*info->file_types));

	for (l = info->bincl_list; l != NULL; l = l->next)
		if (l->hash == hash && strcmp(l->name, name) == 0)
			break;
	if (l == NULL)
	{
		warn_stab(name, _("Undefined N_EXCL"));
		info->file_types[info->files - 1] = NULL;
		return true;
	}

	info->file_types[info->files - 1] = l->file_types;

	return true;
}

/* Handle a variable definition.  gcc emits variable definitions for a
   block before the N_LBRAC, so we must hold onto them until we see
   it.  The SunPRO compiler emits variable definitions after the
   N_LBRAC, so we can call debug_record_variable immediately.  */

static bool
stab_record_variable(void* dhandle, struct stab_handle* info,
	const char* name, debug_type type,
	enum debug_var_kind kind, bfd_vma val)
{
	struct stab_pending_var* v;

	if ((kind == DEBUG_GLOBAL || kind == DEBUG_STATIC)
		|| !info->within_function
		|| (info->gcc_compiled == 0 && info->n_opt_found))
		return debug_record_variable(dhandle, name, type, kind, val);

	v = debug_xzalloc(dhandle, sizeof(*v));

	v->next = info->pending;
	v->name = name;
	v->type = type;
	v->kind = kind;
	v->val = val;
	info->pending = v;

	return true;
}

/* Emit pending variable definitions.  This is called after we see the
   N_LBRAC that starts the block.  */

static bool
stab_emit_pending_vars(void* dhandle, struct stab_handle* info)
{
	struct stab_pending_var* v;

	v = info->pending;
	while (v != NULL)
	{
		if (!debug_record_variable(dhandle, v->name, v->type, v->kind, v->val))
			return false;

		v = v->next;
	}

	info->pending = NULL;

	return true;
}

/* Find the slot for a type in the database.  */

static debug_type*
stab_find_slot(void* dhandle, struct stab_handle* info, const int* typenums)
{
	unsigned int filenum;
	unsigned int tindex;
	unsigned int base_index;
	struct stab_types** ps;

	filenum = typenums[0];
	tindex = typenums[1];

	if (filenum >= info->files)
	{
		fprintf(stderr, _("Type file number %d out of range\n"), filenum);
		return NULL;
	}

	ps = info->file_types + filenum;
	base_index = tindex / STAB_TYPES_SLOTS * STAB_TYPES_SLOTS;
	tindex -= base_index;
	while (*ps && (*ps)->base_index < base_index)
		ps = &(*ps)->next;

	if (*ps == NULL || (*ps)->base_index != base_index)
	{
		struct stab_types* n = debug_xzalloc(dhandle, sizeof(*n));
		n->next = *ps;
		n->base_index = base_index;
		*ps = n;
	}

	return (*ps)->types + tindex;
}

/* Find a type given a type number.  If the type has not been
   allocated yet, create an indirect type.  */

static debug_type
stab_find_type(void* dhandle, struct stab_handle* info, const int* typenums)
{
	debug_type* slot;

	if (typenums[0] == 0 && typenums[1] < 0)
	{
		/* A negative type number indicates an XCOFF builtin type.  */
		return stab_xcoff_builtin_type(dhandle, info, typenums[1]);
	}

	slot = stab_find_slot(dhandle, info, typenums);
	if (slot == NULL)
		return DEBUG_TYPE_NULL;

	if (*slot == DEBUG_TYPE_NULL)
		return debug_make_indirect_type(dhandle, slot, (const char*)NULL);

	return *slot;
}

/* Record that a given type number refers to a given type.  */

static bool
stab_record_type(void* dhandle, struct stab_handle* info,
	const int* typenums, debug_type type)
{
	debug_type* slot;

	slot = stab_find_slot(dhandle, info, typenums);
	if (slot == NULL)
		return false;

	/* gdb appears to ignore type redefinitions, so we do as well.  */

	*slot = type;

	return true;
}

/* Return an XCOFF builtin type.  */

static debug_type
stab_xcoff_builtin_type(void* dhandle, struct stab_handle* info,
	unsigned int typenum)
{
	debug_type rettype;
	const char* name;

	typenum = -typenum - 1;
	if (typenum >= XCOFF_TYPE_COUNT)
	{
		fprintf(stderr, _("Unrecognized XCOFF type %d\n"), -typenum - 1);
		return DEBUG_TYPE_NULL;
	}
	if (info->xcoff_types[typenum] != NULL)
		return info->xcoff_types[typenum];

	switch (typenum)
	{
	case 0:
		/* The size of this and all the other types are fixed, defined
	   by the debugging format.  */
		name = "int";
		rettype = debug_make_int_type(dhandle, 4, false);
		break;
	case 1:
		name = "char";
		rettype = debug_make_int_type(dhandle, 1, false);
		break;
	case 2:
		name = "short";
		rettype = debug_make_int_type(dhandle, 2, false);
		break;
	case 3:
		name = "long long";
		rettype = debug_make_int_type(dhandle, 4, false);
		break;
	case 4:
		name = "unsigned char";
		rettype = debug_make_int_type(dhandle, 1, true);
		break;
	case 5:
		name = "signed char";
		rettype = debug_make_int_type(dhandle, 1, false);
		break;
	case 6:
		name = "unsigned short";
		rettype = debug_make_int_type(dhandle, 2, true);
		break;
	case 7:
		name = "unsigned int";
		rettype = debug_make_int_type(dhandle, 4, true);
		break;
	case 8:
		name = "unsigned";
		rettype = debug_make_int_type(dhandle, 4, true);
		break;
	case 9:
		name = "unsigned long long";
		rettype = debug_make_int_type(dhandle, 4, true);
		break;
	case 10:
		name = "void";
		rettype = debug_make_void_type(dhandle);
		break;
	case 11:
		/* IEEE single precision (32 bit).  */
		name = "float";
		rettype = debug_make_float_type(dhandle, 4);
		break;
	case 12:
		/* IEEE double precision (64 bit).  */
		name = "double";
		rettype = debug_make_float_type(dhandle, 8);
		break;
	case 13:
		/* This is an IEEE double on the RS/6000, and different machines
	   with different sizes for "long long double" should use different
	   negative type numbers.  See stabs.texinfo.  */
		name = "long long double";
		rettype = debug_make_float_type(dhandle, 8);
		break;
	case 14:
		name = "integer";
		rettype = debug_make_int_type(dhandle, 4, false);
		break;
	case 15:
		name = "boolean";
		rettype = debug_make_bool_type(dhandle, 4);
		break;
	case 16:
		name = "short real";
		rettype = debug_make_float_type(dhandle, 4);
		break;
	case 17:
		name = "real";
		rettype = debug_make_float_type(dhandle, 8);
		break;
	case 18:
		/* FIXME */
		name = "stringptr";
		rettype = NULL;
		break;
	case 19:
		/* FIXME */
		name = "character";
		rettype = debug_make_int_type(dhandle, 1, true);
		break;
	case 20:
		name = "logical*1";
		rettype = debug_make_bool_type(dhandle, 1);
		break;
	case 21:
		name = "logical*2";
		rettype = debug_make_bool_type(dhandle, 2);
		break;
	case 22:
		name = "logical*4";
		rettype = debug_make_bool_type(dhandle, 4);
		break;
	case 23:
		name = "logical";
		rettype = debug_make_bool_type(dhandle, 4);
		break;
	case 24:
		/* Complex type consisting of two IEEE single precision values.  */
		name = "complex";
		rettype = debug_make_complex_type(dhandle, 8);
		break;
	case 25:
		/* Complex type consisting of two IEEE double precision values.  */
		name = "double complex";
		rettype = debug_make_complex_type(dhandle, 16);
		break;
	case 26:
		name = "integer*1";
		rettype = debug_make_int_type(dhandle, 1, false);
		break;
	case 27:
		name = "integer*2";
		rettype = debug_make_int_type(dhandle, 2, false);
		break;
	case 28:
		name = "integer*4";
		rettype = debug_make_int_type(dhandle, 4, false);
		break;
	case 29:
		/* FIXME */
		name = "wchar";
		rettype = debug_make_int_type(dhandle, 2, false);
		break;
	case 30:
		name = "long long";
		rettype = debug_make_int_type(dhandle, 8, false);
		break;
	case 31:
		name = "unsigned long long";
		rettype = debug_make_int_type(dhandle, 8, true);
		break;
	case 32:
		name = "logical*8";
		rettype = debug_make_bool_type(dhandle, 8);
		break;
	case 33:
		name = "integer*8";
		rettype = debug_make_int_type(dhandle, 8, false);
		break;
	default:
		abort();
	}

	rettype = debug_name_type(dhandle, name, rettype);
	info->xcoff_types[typenum] = rettype;
	return rettype;
}

/* Find or create a tagged type.  */

static debug_type
stab_find_tagged_type(void* dhandle, struct stab_handle* info,
	const char* p, int len, enum debug_type_kind kind)
{
	char* name;
	debug_type dtype;
	struct stab_tag* st;

	name = savestring(dhandle, p, len);

	/* We pass DEBUG_KIND_ILLEGAL because we want all tags in the same
	   namespace.  This is right for C, and I don't know how to handle
	   other languages.  FIXME.  */
	dtype = debug_find_tagged_type(dhandle, name, DEBUG_KIND_ILLEGAL);
	if (dtype != DEBUG_TYPE_NULL)
		return dtype;

	/* We need to allocate an entry on the undefined tag list.  */
	for (st = info->tags; st != NULL; st = st->next)
	{
		if (st->name[0] == name[0]
			&& strcmp(st->name, name) == 0)
		{
			if (st->kind == DEBUG_KIND_ILLEGAL)
				st->kind = kind;
			break;
		}
	}
	if (st == NULL)
	{
		st = debug_xzalloc(dhandle, sizeof(*st));

		st->next = info->tags;
		st->name = name;
		st->kind = kind;
		st->slot = DEBUG_TYPE_NULL;
		st->type = debug_make_indirect_type(dhandle, &st->slot, name);
		info->tags = st;
	}

	return st->type;
}

/* In order to get the correct argument types for a stubbed method, we
   need to extract the argument types from a C++ mangled string.
   Since the argument types can refer back to the return type, this
   means that we must demangle the entire physical name.  In gdb this
   is done by calling cplus_demangle and running the results back
   through the C++ expression parser.  Since we have no expression
   parser, we must duplicate much of the work of cplus_demangle here.

   We assume that GNU style demangling is used, since this is only
   done for method stubs, and only g++ should output that form of
   debugging information.  */

   /* This structure is used to hold a pointer to type information which
	  demangling a string.  */

struct stab_demangle_typestring
{
	/* The start of the type.  This is not null terminated.  */
	const char* typestring;
	/* The length of the type.  */
	unsigned int len;
};

/* This structure is used to hold information while demangling a
   string.  */

struct stab_demangle_info
{
	/* The debugging information handle.  */
	void* dhandle;
	/* The stab information handle.  */
	struct stab_handle* info;
	/* The array of arguments we are building.  */
	debug_type* args;
	/* Whether the method takes a variable number of arguments.  */
	bool varargs;
	/* The array of types we have remembered.  */
	struct stab_demangle_typestring* typestrings;
	/* The number of typestrings.  */
	unsigned int typestring_count;
	/* The number of typestring slots we have allocated.  */
	unsigned int typestring_alloc;
};

static void stab_bad_demangle(const char*);
static unsigned int stab_demangle_count(const char**);
static bool stab_demangle_get_count(const char**, unsigned int*);
static bool stab_demangle_prefix
(struct stab_demangle_info*, const char**, unsigned int);
static bool stab_demangle_function_name
(struct stab_demangle_info*, const char**, const char*);
static bool stab_demangle_signature
(struct stab_demangle_info*, const char**);
static bool stab_demangle_qualified
(struct stab_demangle_info*, const char**, debug_type*);
static bool stab_demangle_template
(struct stab_demangle_info*, const char**, char**);
static bool stab_demangle_class
(struct stab_demangle_info*, const char**, const char**);
static bool stab_demangle_args
(struct stab_demangle_info*, const char**, debug_type**, bool*);
static bool stab_demangle_arg
(struct stab_demangle_info*, const char**, debug_type**,
	unsigned int*, unsigned int*);
static bool stab_demangle_type
(struct stab_demangle_info*, const char**, debug_type*);
static bool stab_demangle_fund_type
(struct stab_demangle_info*, const char**, debug_type*);
static bool stab_demangle_remember_type
(struct stab_demangle_info*, const char*, int);

/* Warn about a bad demangling.  */

static void
stab_bad_demangle(const char* s)
{
	fprintf(stderr, _("bad mangled name `%s'\n"), s);
}

/* Get a count from a stab string.  */

static unsigned int
stab_demangle_count(const char** pp)
{
	unsigned int count;

	count = 0;
	while (ISDIGIT(**pp))
	{
		count *= 10;
		count += **pp - '0';
		++* pp;
	}
	return count;
}

/* Require a count in a string.  The count may be multiple digits, in
   which case it must end in an underscore.  */

static bool
stab_demangle_get_count(const char** pp, unsigned int* pi)
{
	if (!ISDIGIT(**pp))
		return false;

	*pi = **pp - '0';
	++* pp;
	if (ISDIGIT(**pp))
	{
		unsigned int count;
		const char* p;

		count = *pi;
		p = *pp;
		do
		{
			count *= 10;
			count += *p - '0';
			++p;
		} while (ISDIGIT(*p));
		if (*p == '_')
		{
			*pp = p + 1;
			*pi = count;
		}
	}

	return true;
}

/* This function demangles a physical name, returning a NULL
   terminated array of argument types.  */

static debug_type*
stab_demangle_argtypes(void* dhandle, struct stab_handle* info,
	const char* physname, bool* pvarargs,
	unsigned int physname_len)
{
	struct stab_demangle_info minfo;

	/* Check for the g++ V3 ABI.  */
	if (physname[0] == '_' && physname[1] == 'Z')
		return stab_demangle_v3_argtypes(dhandle, info, physname, pvarargs);

	minfo.dhandle = dhandle;
	minfo.info = info;
	minfo.args = NULL;
	minfo.varargs = false;
	minfo.typestring_alloc = 10;
	minfo.typestrings
		= xmalloc(minfo.typestring_alloc * sizeof(*minfo.typestrings));
	minfo.typestring_count = 0;

	/* cplus_demangle checks for special GNU mangled forms, but we can't
	   see any of them in mangled method argument types.  */

	if (!stab_demangle_prefix(&minfo, &physname, physname_len))
		goto error_return;

	if (*physname != '\0')
	{
		if (!stab_demangle_signature(&minfo, &physname))
			goto error_return;
	}

	free(minfo.typestrings);

	if (minfo.args == NULL)
		fprintf(stderr, _("no argument types in mangled string\n"));

	*pvarargs = minfo.varargs;
	return minfo.args;

error_return:
	free(minfo.typestrings);
	return NULL;
}

/* Demangle the prefix of the mangled name.  */

static bool
stab_demangle_prefix(struct stab_demangle_info* minfo, const char** pp,
	unsigned int physname_len)
{
	const char* scan;
	unsigned int i;

	/* cplus_demangle checks for global constructors and destructors,
	   but we can't see them in mangled argument types.  */

	if (physname_len)
		scan = *pp + physname_len;
	else
	{
		/* Look for `__'.  */
		scan = *pp;
		do
			scan = strchr(scan, '_');
		while (scan != NULL && *++scan != '_');

		if (scan == NULL)
		{
			stab_bad_demangle(*pp);
			return false;
		}

		--scan;

		/* We found `__'; move ahead to the last contiguous `__' pair.  */
		i = strspn(scan, "_");
		if (i > 2)
			scan += i - 2;
	}

	if (scan == *pp
		&& (ISDIGIT(scan[2])
			|| scan[2] == 'Q'
			|| scan[2] == 't'))
	{
		/* This is a GNU style constructor name.  */
		*pp = scan + 2;
		return true;
	}
	else if (scan == *pp
		&& !ISDIGIT(scan[2])
		&& scan[2] != 't')
	{
		/* Look for the `__' that separates the prefix from the
		   signature.  */
		while (*scan == '_')
			++scan;
		scan = strstr(scan, "__");
		if (scan == NULL || scan[2] == '\0')
		{
			stab_bad_demangle(*pp);
			return false;
		}

		return stab_demangle_function_name(minfo, pp, scan);
	}
	else if (scan[2] != '\0')
	{
		/* The name doesn't start with `__', but it does contain `__'.  */
		return stab_demangle_function_name(minfo, pp, scan);
	}
	else
	{
		stab_bad_demangle(*pp);
		return false;
	}
	/*NOTREACHED*/
}

/* Demangle a function name prefix.  The scan argument points to the
   double underscore which separates the function name from the
   signature.  */

static bool
stab_demangle_function_name(struct stab_demangle_info* minfo,
	const char** pp, const char* scan)
{
	const char* name;

	/* The string from *pp to scan is the name of the function.  We
	   don't care about the name, since we just looking for argument
	   types.  However, for conversion operators, the name may include a
	   type which we must remember in order to handle backreferences.  */

	name = *pp;
	*pp = scan + 2;

	if (*pp - name >= 5
		&& startswith(name, "type")
		&& (name[4] == '$' || name[4] == '.'))
	{
		const char* tem;

		/* This is a type conversion operator.  */
		tem = name + 5;
		if (!stab_demangle_type(minfo, &tem, (debug_type*)NULL))
			return false;
	}
	else if (name[0] == '_'
		&& name[1] == '_'
		&& name[2] == 'o'
		&& name[3] == 'p')
	{
		const char* tem;

		/* This is a type conversion operator.  */
		tem = name + 4;
		if (!stab_demangle_type(minfo, &tem, (debug_type*)NULL))
			return false;
	}

	return true;
}

/* Demangle the signature.  This is where the argument types are
   found.  */

static bool
stab_demangle_signature(struct stab_demangle_info* minfo, const char** pp)
{
	const char* orig;
	bool expect_func, func_done;
	const char* hold;

	orig = *pp;

	expect_func = false;
	func_done = false;
	hold = NULL;

	while (**pp != '\0')
	{
		switch (**pp)
		{
		case 'Q':
			hold = *pp;
			if (!stab_demangle_qualified(minfo, pp, (debug_type*)NULL)
				|| !stab_demangle_remember_type(minfo, hold, *pp - hold))
				return false;
			expect_func = true;
			hold = NULL;
			break;

		case 'S':
			/* Static member function.  FIXME: Can this happen?  */
			if (hold == NULL)
				hold = *pp;
			++* pp;
			break;

		case 'C':
			/* Const member function.  */
			if (hold == NULL)
				hold = *pp;
			++* pp;
			break;

		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			if (hold == NULL)
				hold = *pp;
			if (!stab_demangle_class(minfo, pp, (const char**)NULL)
				|| !stab_demangle_remember_type(minfo, hold, *pp - hold))
				return false;
			expect_func = true;
			hold = NULL;
			break;

		case 'F':
			/* Function.  I don't know if this actually happens with g++
				   output.  */
			hold = NULL;
			func_done = true;
			++* pp;
			if (!stab_demangle_args(minfo, pp, &minfo->args, &minfo->varargs))
				return false;
			break;

		case 't':
			/* Template.  */
			if (hold == NULL)
				hold = *pp;
			if (!stab_demangle_template(minfo, pp, (char**)NULL)
				|| !stab_demangle_remember_type(minfo, hold, *pp - hold))
				return false;
			hold = NULL;
			expect_func = true;
			break;

		case '_':
			/* At the outermost level, we cannot have a return type
			   specified, so if we run into another '_' at this point we
			   are dealing with a mangled name that is either bogus, or
			   has been mangled by some algorithm we don't know how to
			   deal with.  So just reject the entire demangling.  */
			stab_bad_demangle(orig);
			return false;

		default:
			/* Assume we have stumbled onto the first outermost function
			   argument token, and start processing args.  */
			func_done = true;
			if (!stab_demangle_args(minfo, pp, &minfo->args, &minfo->varargs))
				return false;
			break;
		}

		if (expect_func)
		{
			func_done = true;
			if (!stab_demangle_args(minfo, pp, &minfo->args, &minfo->varargs))
				return false;
		}
	}

	if (!func_done)
	{
		/* With GNU style demangling, bar__3foo is 'foo::bar(void)', and
	   bar__3fooi is 'foo::bar(int)'.  We get here when we find the
	   first case, and need to ensure that the '(void)' gets added
	   to the current declp.  */
		if (!stab_demangle_args(minfo, pp, &minfo->args, &minfo->varargs))
			return false;
	}

	return true;
}

/* Demangle a qualified name, such as "Q25Outer5Inner" which is the
   mangled form of "Outer::Inner".  */

static bool
stab_demangle_qualified(struct stab_demangle_info* minfo, const char** pp,
	debug_type* ptype)
{
	const char* orig;
	const char* p;
	unsigned int qualifiers;
	debug_type context;

	orig = *pp;

	switch ((*pp)[1])
	{
	case '_':
		/* GNU mangled name with more than 9 classes.  The count is
	   preceded by an underscore (to distinguish it from the <= 9
	   case) and followed by an underscore.  */
		p = *pp + 2;
		if (!ISDIGIT(*p) || *p == '0')
		{
			stab_bad_demangle(orig);
			return false;
		}
		qualifiers = atoi(p);
		while (ISDIGIT(*p))
			++p;
		if (*p != '_')
		{
			stab_bad_demangle(orig);
			return false;
		}
		*pp = p + 1;
		break;

	case '1': case '2': case '3': case '4': case '5':
	case '6': case '7': case '8': case '9':
		qualifiers = (*pp)[1] - '0';
		/* Skip an optional underscore after the count.  */
		if ((*pp)[2] == '_')
			++* pp;
		*pp += 2;
		break;

	case '0':
	default:
		stab_bad_demangle(orig);
		return false;
	}

	context = DEBUG_TYPE_NULL;

	/* Pick off the names.  */
	while (qualifiers-- > 0)
	{
		if (**pp == '_')
			++* pp;
		if (**pp == 't')
		{
			char* name;

			if (!stab_demangle_template(minfo, pp,
				ptype != NULL ? &name : NULL))
				return false;

			if (ptype != NULL)
			{
				context = stab_find_tagged_type(minfo->dhandle, minfo->info,
					name, strlen(name),
					DEBUG_KIND_CLASS);
				if (context == DEBUG_TYPE_NULL)
					return false;
			}
		}
		else
		{
			unsigned int len;

			len = stab_demangle_count(pp);
			if (strlen(*pp) < len)
			{
				stab_bad_demangle(orig);
				return false;
			}

			if (ptype != NULL)
			{
				const debug_field* fields;

				fields = NULL;
				if (context != DEBUG_TYPE_NULL)
					fields = debug_get_fields(minfo->dhandle, context);

				context = DEBUG_TYPE_NULL;

				if (fields != NULL)
				{
					char* name;

					/* Try to find the type by looking through the
							   fields of context until we find a field with the
							   same type.  This ought to work for a class
							   defined within a class, but it won't work for,
							   e.g., an enum defined within a class.  stabs does
							   not give us enough information to figure out the
							   latter case.  */

					name = savestring(minfo->dhandle, *pp, len);

					for (; *fields != DEBUG_FIELD_NULL; fields++)
					{
						debug_type ft;
						const char* dn;

						ft = debug_get_field_type(minfo->dhandle, *fields);
						if (ft == NULL)
							return false;
						dn = debug_get_type_name(minfo->dhandle, ft);
						if (dn != NULL && strcmp(dn, name) == 0)
						{
							context = ft;
							break;
						}
					}
				}

				if (context == DEBUG_TYPE_NULL)
				{
					/* We have to fall back on finding the type by name.
							   If there are more types to come, then this must
							   be a class.  Otherwise, it could be anything.  */

					if (qualifiers == 0)
					{
						char* name;

						name = savestring(minfo->dhandle, *pp, len);
						context = debug_find_named_type(minfo->dhandle,
							name);
					}

					if (context == DEBUG_TYPE_NULL)
					{
						context = stab_find_tagged_type(minfo->dhandle,
							minfo->info,
							*pp, len,
							(qualifiers == 0
								? DEBUG_KIND_ILLEGAL
								: DEBUG_KIND_CLASS));
						if (context == DEBUG_TYPE_NULL)
							return false;
					}
				}
			}

			*pp += len;
		}
	}

	if (ptype != NULL)
		*ptype = context;

	return true;
}

/* Demangle a template.  If PNAME is not NULL, this sets *PNAME to a
   string representation of the template.  */

static bool
stab_demangle_template(struct stab_demangle_info* minfo, const char** pp,
	char** pname)
{
	const char* orig;
	unsigned int r, i;

	orig = *pp;

	++* pp;

	/* Skip the template name.  */
	r = stab_demangle_count(pp);
	if (r == 0 || strlen(*pp) < r)
	{
		stab_bad_demangle(orig);
		return false;
	}
	*pp += r;

	/* Get the size of the parameter list.  */
	if (stab_demangle_get_count(pp, &r) == 0)
	{
		stab_bad_demangle(orig);
		return false;
	}

	for (i = 0; i < r; i++)
	{
		if (**pp == 'Z')
		{
			/* This is a type parameter.  */
			++* pp;
			if (!stab_demangle_type(minfo, pp, (debug_type*)NULL))
				return false;
		}
		else
		{
			const char* old_p;
			bool pointerp, realp, integralp, charp, boolp;
			bool done;

			old_p = *pp;
			pointerp = false;
			realp = false;
			integralp = false;
			charp = false;
			boolp = false;
			done = false;

			/* This is a value parameter.  */

			if (!stab_demangle_type(minfo, pp, (debug_type*)NULL))
				return false;

			while (*old_p != '\0' && !done)
			{
				switch (*old_p)
				{
				case 'P':
				case 'p':
				case 'R':
					pointerp = true;
					done = true;
					break;
				case 'C':	/* Const.  */
				case 'S':	/* Signed.  */
				case 'U':	/* Unsigned.  */
				case 'V':	/* Volatile.  */
				case 'F':	/* Function.  */
				case 'M':	/* Member function.  */
				case 'O':	/* ??? */
					++old_p;
					break;
				case 'Q':	/* Qualified name.  */
					integralp = true;
					done = true;
					break;
				case 'T':	/* Remembered type.  */
					abort();
				case 'v':	/* Void.  */
					abort();
				case 'x':	/* Long long.  */
				case 'l':	/* long long.  */
				case 'i':	/* Int.  */
				case 's':	/* Short.  */
				case 'w':	/* Wchar_t.  */
					integralp = true;
					done = true;
					break;
				case 'b':	/* Bool.  */
					boolp = true;
					done = true;
					break;
				case 'c':	/* Char.  */
					charp = true;
					done = true;
					break;
				case 'r':	/* long long double.  */
				case 'd':	/* Double.  */
				case 'f':	/* Float.  */
					realp = true;
					done = true;
					break;
				default:
					/* Assume it's a user defined integral type.  */
					integralp = true;
					done = true;
					break;
				}
			}

			if (integralp)
			{
				if (**pp == 'm')
					++* pp;
				while (ISDIGIT(**pp))
					++* pp;
			}
			else if (charp)
			{
				unsigned int val;

				if (**pp == 'm')
					++* pp;
				val = stab_demangle_count(pp);
				if (val == 0)
				{
					stab_bad_demangle(orig);
					return false;
				}
			}
			else if (boolp)
			{
				unsigned int val;

				val = stab_demangle_count(pp);
				if (val != 0 && val != 1)
				{
					stab_bad_demangle(orig);
					return false;
				}
			}
			else if (realp)
			{
				if (**pp == 'm')
					++* pp;
				while (ISDIGIT(**pp))
					++* pp;
				if (**pp == '.')
				{
					++* pp;
					while (ISDIGIT(**pp))
						++* pp;
				}
				if (**pp == 'e')
				{
					++* pp;
					while (ISDIGIT(**pp))
						++* pp;
				}
			}
			else if (pointerp)
			{
				unsigned int len;

				len = stab_demangle_count(pp);
				if (len == 0)
				{
					stab_bad_demangle(orig);
					return false;
				}
				*pp += len;
			}
		}
	}

	/* We can translate this to a string fairly easily by invoking the
	   regular demangling routine.  */
	if (pname != NULL)
	{
		char* s1, * s2, * s3, * s4 = NULL;
		char* from, * to;

		s1 = savestring(minfo->dhandle, orig, *pp - orig);

		s2 = concat("NoSuchStrinG__", s1, (const char*)NULL);

		s3 = cplus_demangle(s2, demangle_flags);

		free(s2);

		if (s3 != NULL)
			s4 = strstr(s3, "::NoSuchStrinG");
		if (s3 == NULL || s4 == NULL)
		{
			stab_bad_demangle(orig);
			free(s3);
			return false;
		}

		/* Eliminating all spaces, except those between > characters,
		   makes it more likely that the demangled name will match the
		   name which g++ used as the structure name.  */
		for (from = to = s3; from != s4; ++from)
			if (*from != ' '
				|| (from[1] == '>' && from > s3 && from[-1] == '>'))
				*to++ = *from;

		*pname = savestring(minfo->dhandle, s3, to - s3);

		free(s3);
	}

	return true;
}

/* Demangle a class name.  */

static bool
stab_demangle_class(struct stab_demangle_info* minfo ATTRIBUTE_UNUSED,
	const char** pp, const char** pstart)
{
	const char* orig;
	unsigned int n;

	orig = *pp;

	n = stab_demangle_count(pp);
	if (strlen(*pp) < n)
	{
		stab_bad_demangle(orig);
		return false;
	}

	if (pstart != NULL)
		*pstart = *pp;

	*pp += n;

	return true;
}

/* Demangle function arguments.  If the pargs argument is not NULL, it
   is set to a NULL terminated array holding the arguments.  */

static bool
stab_demangle_args(struct stab_demangle_info* minfo, const char** pp,
	debug_type** pargs, bool* pvarargs)
{
	const char* orig;
	unsigned int alloc, count;

	orig = *pp;

	alloc = 10;
	if (pargs != NULL)
		*pargs = xmalloc(alloc * sizeof(**pargs));
	if (pvarargs != NULL)
		*pvarargs = false;
	count = 0;

	while (**pp != '_' && **pp != '\0' && **pp != 'e')
	{
		if (**pp == 'N' || **pp == 'T')
		{
			char temptype;
			unsigned int r, t;

			temptype = **pp;
			++* pp;

			if (temptype == 'T')
				r = 1;
			else
			{
				if (!stab_demangle_get_count(pp, &r))
					goto bad;
			}

			if (!stab_demangle_get_count(pp, &t)
				|| t >= minfo->typestring_count)
				goto bad;

			while (r-- > 0)
			{
				const char* tem;

				tem = minfo->typestrings[t].typestring;
				if (!stab_demangle_arg(minfo, &tem, pargs, &count, &alloc))
					goto fail;
			}
		}
		else
		{
			if (!stab_demangle_arg(minfo, pp, pargs, &count, &alloc))
				goto fail;
		}
	}

	if (pargs != NULL)
	{
		debug_type* xargs;
		(*pargs)[count] = DEBUG_TYPE_NULL;
		xargs = debug_xalloc(minfo->dhandle, (count + 1) * sizeof(*xargs));
		memcpy(xargs, *pargs, (count + 1) * sizeof(*xargs));
		free(*pargs);
		*pargs = xargs;
	}

	if (**pp == 'e')
	{
		if (pvarargs != NULL)
			*pvarargs = true;
		++* pp;
	}
	return true;

bad:
	stab_bad_demangle(orig);
fail:
	if (pargs != NULL)
	{
		free(*pargs);
		*pargs = NULL;
	}
	return false;
}

/* Demangle a single argument.  */

static bool
stab_demangle_arg(struct stab_demangle_info* minfo, const char** pp,
	debug_type** pargs, unsigned int* pcount,
	unsigned int* palloc)
{
	const char* start;
	debug_type type;

	start = *pp;
	if (!stab_demangle_type(minfo, pp,
		pargs == NULL ? (debug_type*)NULL : &type)
		|| !stab_demangle_remember_type(minfo, start, *pp - start))
		return false;

	if (pargs != NULL)
	{
		if (type == DEBUG_TYPE_NULL)
			return false;

		if (*pcount + 1 >= *palloc)
		{
			*palloc += 10;
			*pargs = xrealloc(*pargs, *palloc * sizeof(**pargs));
		}
		(*pargs)[*pcount] = type;
		++* pcount;
	}

	return true;
}

/* Demangle a type.  If the ptype argument is not NULL, *ptype is set
   to the newly allocated type.  */

static bool
stab_demangle_type(struct stab_demangle_info* minfo, const char** pp,
	debug_type* ptype)
{
	const char* orig;

	orig = *pp;

	switch (**pp)
	{
	case 'P':
	case 'p':
		/* A pointer type.  */
		++ * pp;
		if (!stab_demangle_type(minfo, pp, ptype))
			return false;
		if (ptype != NULL)
			*ptype = debug_make_pointer_type(minfo->dhandle, *ptype);
		break;

	case 'R':
		/* A reference type.  */
		++ * pp;
		if (!stab_demangle_type(minfo, pp, ptype))
			return false;
		if (ptype != NULL)
			*ptype = debug_make_reference_type(minfo->dhandle, *ptype);
		break;

	case 'A':
		/* An array.  */
	{
		unsigned long long high;

		++* pp;
		high = 0;
		while (**pp != '\0' && **pp != '_')
		{
			if (!ISDIGIT(**pp))
			{
				stab_bad_demangle(orig);
				return false;
			}
			high *= 10;
			high += **pp - '0';
			++* pp;
		}
		if (**pp != '_')
		{
			stab_bad_demangle(orig);
			return false;
		}
		++* pp;

		if (!stab_demangle_type(minfo, pp, ptype))
			return false;
		if (ptype != NULL)
		{
			debug_type int_type;

			int_type = debug_find_named_type(minfo->dhandle, "int");
			if (int_type == NULL)
				int_type = debug_make_int_type(minfo->dhandle, 4, false);
			*ptype = debug_make_array_type(minfo->dhandle, *ptype, int_type,
				0, high, false);
		}
	}
	break;

	case 'T':
		/* A back reference to a remembered type.  */
	{
		unsigned int i;
		const char* p;

		++* pp;
		if (!stab_demangle_get_count(pp, &i))
		{
			stab_bad_demangle(orig);
			return false;
		}
		if (i >= minfo->typestring_count)
		{
			stab_bad_demangle(orig);
			return false;
		}
		p = minfo->typestrings[i].typestring;
		if (!stab_demangle_type(minfo, &p, ptype))
			return false;
	}
	break;

	case 'F':
		/* A function.  */
	{
		debug_type* args;
		bool varargs;

		++* pp;
		if (!stab_demangle_args(minfo, pp,
			(ptype == NULL
				? (debug_type**)NULL
				: &args),
			(ptype == NULL
				? (bool*)NULL
				: &varargs)))
			return false;
		if (**pp != '_')
		{
			/* cplus_demangle will accept a function without a return
			   type, but I don't know when that will happen, or what
			   to do if it does.  */
			stab_bad_demangle(orig);
			return false;
		}
		++* pp;
		if (!stab_demangle_type(minfo, pp, ptype))
			return false;
		if (ptype != NULL)
			*ptype = debug_make_function_type(minfo->dhandle, *ptype, args,
				varargs);

	}
	break;

	case 'M':
	case 'O':
	{
		bool memberp;
		debug_type class_type = DEBUG_TYPE_NULL;
		debug_type* args;
		bool varargs;
		unsigned int n;
		const char* name;

		memberp = **pp == 'M';
		args = NULL;
		varargs = false;

		++* pp;
		if (ISDIGIT(**pp))
		{
			n = stab_demangle_count(pp);
			if (strlen(*pp) < n)
			{
				stab_bad_demangle(orig);
				return false;
			}
			name = *pp;
			*pp += n;

			if (ptype != NULL)
			{
				class_type = stab_find_tagged_type(minfo->dhandle,
					minfo->info,
					name, (int)n,
					DEBUG_KIND_CLASS);
				if (class_type == DEBUG_TYPE_NULL)
					return false;
			}
		}
		else if (**pp == 'Q')
		{
			if (!stab_demangle_qualified(minfo, pp,
				(ptype == NULL
					? (debug_type*)NULL
					: &class_type)))
				return false;
		}
		else
		{
			stab_bad_demangle(orig);
			return false;
		}

		if (memberp)
		{
			if (**pp == 'C')
			{
				++* pp;
			}
			else if (**pp == 'V')
			{
				++* pp;
			}
			if (**pp != 'F')
			{
				stab_bad_demangle(orig);
				return false;
			}
			++* pp;
			if (!stab_demangle_args(minfo, pp,
				(ptype == NULL
					? (debug_type**)NULL
					: &args),
				(ptype == NULL
					? (bool*)NULL
					: &varargs)))
				return false;
		}

		if (**pp != '_')
		{
			stab_bad_demangle(orig);
			return false;
		}
		++* pp;

		if (!stab_demangle_type(minfo, pp, ptype))
			return false;

		if (ptype != NULL)
		{
			if (!memberp)
				*ptype = debug_make_offset_type(minfo->dhandle, class_type,
					*ptype);
			else
			{
				/* FIXME: We have no way to record constp or
						   volatilep.  */
				*ptype = debug_make_method_type(minfo->dhandle, *ptype,
					class_type, args, varargs);
			}
		}
	}
	break;

	case 'G':
		++ * pp;
		if (!stab_demangle_type(minfo, pp, ptype))
			return false;
		break;

	case 'C':
		++ * pp;
		if (!stab_demangle_type(minfo, pp, ptype))
			return false;
		if (ptype != NULL)
			*ptype = debug_make_const_type(minfo->dhandle, *ptype);
		break;

	case 'Q':
	{
		if (!stab_demangle_qualified(minfo, pp, ptype))
			return false;
	}
	break;

	default:
		if (!stab_demangle_fund_type(minfo, pp, ptype))
			return false;
		break;
	}

	return true;
}

/* Demangle a fundamental type.  If the ptype argument is not NULL,
   *ptype is set to the newly allocated type.  */

static bool
stab_demangle_fund_type(struct stab_demangle_info* minfo, const char** pp,
	debug_type* ptype)
{
	const char* orig;
	bool constp, volatilep, unsignedp, signedp;
	bool done;

	orig = *pp;

	constp = false;
	volatilep = false;
	unsignedp = false;
	signedp = false;

	done = false;
	while (!done)
	{
		switch (**pp)
		{
		case 'C':
			constp = true;
			++* pp;
			break;

		case 'U':
			unsignedp = true;
			++* pp;
			break;

		case 'S':
			signedp = true;
			++* pp;
			break;

		case 'V':
			volatilep = true;
			++* pp;
			break;

		default:
			done = true;
			break;
		}
	}

	switch (**pp)
	{
	case '\0':
	case '_':
		/* cplus_demangle permits this, but I don't know what it means.  */
		stab_bad_demangle(orig);
		break;

	case 'v': /* void */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle, "void");
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_void_type(minfo->dhandle);
		}
		++* pp;
		break;

	case 'x': /* long long */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle,
				(unsignedp
					? "long long unsigned int"
					: "long long int"));
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_int_type(minfo->dhandle, 8, unsignedp);
		}
		++* pp;
		break;

	case 'l': /* long long */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle,
				(unsignedp
					? "long long unsigned int"
					: "long long int"));
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_int_type(minfo->dhandle, 4, unsignedp);
		}
		++* pp;
		break;

	case 'i': /* int */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle,
				(unsignedp
					? "unsigned int"
					: "int"));
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_int_type(minfo->dhandle, 4, unsignedp);
		}
		++* pp;
		break;

	case 's': /* short */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle,
				(unsignedp
					? "short unsigned int"
					: "short int"));
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_int_type(minfo->dhandle, 2, unsignedp);
		}
		++* pp;
		break;

	case 'b': /* bool */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle, "bool");
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_bool_type(minfo->dhandle, 4);
		}
		++* pp;
		break;

	case 'c': /* char */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle,
				(unsignedp
					? "unsigned char"
					: (signedp
						? "signed char"
						: "char")));
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_int_type(minfo->dhandle, 1, unsignedp);
		}
		++* pp;
		break;

	case 'w': /* wchar_t */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle, "__wchar_t");
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_int_type(minfo->dhandle, 2, true);
		}
		++* pp;
		break;

	case 'r': /* long long double */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle, "long long double");
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_float_type(minfo->dhandle, 8);
		}
		++* pp;
		break;

	case 'd': /* double */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle, "double");
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_float_type(minfo->dhandle, 8);
		}
		++* pp;
		break;

	case 'f': /* float */
		if (ptype != NULL)
		{
			*ptype = debug_find_named_type(minfo->dhandle, "float");
			if (*ptype == DEBUG_TYPE_NULL)
				*ptype = debug_make_float_type(minfo->dhandle, 4);
		}
		++* pp;
		break;

	case 'G':
		++ * pp;
		if (!ISDIGIT(**pp))
		{
			stab_bad_demangle(orig);
			return false;
		}
		/* Fall through.  */
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
	{
		const char* hold;

		if (!stab_demangle_class(minfo, pp, &hold))
			return false;
		if (ptype != NULL)
		{
			char* name;

			name = savestring(minfo->dhandle, hold, *pp - hold);
			*ptype = debug_find_named_type(minfo->dhandle, name);
			if (*ptype == DEBUG_TYPE_NULL)
			{
				/* FIXME: It is probably incorrect to assume that
						   undefined types are tagged types.  */
				*ptype = stab_find_tagged_type(minfo->dhandle, minfo->info,
					hold, *pp - hold,
					DEBUG_KIND_ILLEGAL);
				if (*ptype == DEBUG_TYPE_NULL)
					return false;
			}
		}
	}
	break;

	case 't':
	{
		char* name;

		if (!stab_demangle_template(minfo, pp,
			ptype != NULL ? &name : NULL))
			return false;
		if (ptype != NULL)
		{
			*ptype = stab_find_tagged_type(minfo->dhandle, minfo->info,
				name, strlen(name),
				DEBUG_KIND_CLASS);
			if (*ptype == DEBUG_TYPE_NULL)
				return false;
		}
	}
	break;

	default:
		stab_bad_demangle(orig);
		return false;
	}

	if (ptype != NULL)
	{
		if (constp)
			*ptype = debug_make_const_type(minfo->dhandle, *ptype);
		if (volatilep)
			*ptype = debug_make_volatile_type(minfo->dhandle, *ptype);
	}

	return true;
}

/* Remember a type string in a demangled string.  */

static bool
stab_demangle_remember_type(struct stab_demangle_info* minfo,
	const char* p, int len)
{
	if (minfo->typestring_count >= minfo->typestring_alloc)
	{
		minfo->typestring_alloc += 10;
		minfo->typestrings
			= xrealloc(minfo->typestrings,
				minfo->typestring_alloc * sizeof(*minfo->typestrings));
	}

	minfo->typestrings[minfo->typestring_count].typestring = p;
	minfo->typestrings[minfo->typestring_count].len = (unsigned int)len;
	++minfo->typestring_count;

	return true;
}

/* Demangle names encoded using the g++ V3 ABI.  The newer versions of
   g++ which use this ABI do not encode ordinary method argument types
   in a mangled name; they simply output the argument types.  However,
   for a static method, g++ simply outputs the return type and the
   physical name.  So in that case we need to demangle the name here.
   Here PHYSNAME is the physical name of the function, and we set the
   variable pointed at by PVARARGS to indicate whether this function
   is varargs.  This returns NULL, or a NULL terminated array of
   argument types.  */

static debug_type*
stab_demangle_v3_argtypes(void* dhandle, struct stab_handle* info,
	const char* physname, bool* pvarargs)
{
	struct demangle_component* dc;
	void* mem;
	debug_type* pargs;

	dc = cplus_demangle_v3_components(physname, DMGL_PARAMS | demangle_flags, &mem);
	if (dc == NULL)
	{
		stab_bad_demangle(physname);
		return NULL;
	}

	/* We expect to see TYPED_NAME, and the right subtree describes the
	   function type.  */
	if (dc->type != DEMANGLE_COMPONENT_TYPED_NAME
		|| dc->u.s_binary.right->type != DEMANGLE_COMPONENT_FUNCTION_TYPE)
	{
		fprintf(stderr, _("Demangled name is not a function\n"));
		free(mem);
		return NULL;
	}

	pargs = stab_demangle_v3_arglist(dhandle, info,
		dc->u.s_binary.right->u.s_binary.right,
		pvarargs);

	free(mem);

	return pargs;
}

/* Demangle an argument list in a struct demangle_component tree.
   Returns a DEBUG_TYPE_NULL terminated array of argument types, and
   sets *PVARARGS to indicate whether this is a varargs function.  */

static debug_type*
stab_demangle_v3_arglist(void* dhandle, struct stab_handle* info,
	struct demangle_component* arglist,
	bool* pvarargs)
{
	struct demangle_component* dc;
	unsigned int alloc, count;
	debug_type* pargs, * xargs;

	alloc = 10;
	pargs = xmalloc(alloc * sizeof(*pargs));
	*pvarargs = false;

	count = 0;

	for (dc = arglist;
		dc != NULL;
		dc = dc->u.s_binary.right)
	{
		debug_type arg;
		bool varargs;

		if (dc->type != DEMANGLE_COMPONENT_ARGLIST)
		{
			fprintf(stderr, _("Unexpected type in v3 arglist demangling\n"));
			free(pargs);
			return NULL;
		}

		/* PR 13925: Cope if the demangler returns an empty
	   context for a function with no arguments.  */
		if (dc->u.s_binary.left == NULL)
			break;

		arg = stab_demangle_v3_arg(dhandle, info, dc->u.s_binary.left,
			NULL, &varargs);
		if (arg == NULL)
		{
			if (varargs)
			{
				*pvarargs = true;
				continue;
			}
			free(pargs);
			return NULL;
		}

		if (count + 1 >= alloc)
		{
			alloc += 10;
			pargs = xrealloc(pargs, alloc * sizeof(*pargs));
		}

		pargs[count] = arg;
		++count;
	}

	pargs[count] = DEBUG_TYPE_NULL;
	xargs = debug_xalloc(dhandle, (count + 1) * sizeof(*pargs));
	memcpy(xargs, pargs, (count + 1) * sizeof(*pargs));
	free(pargs);

	return xargs;
}

/* Convert a struct demangle_component tree describing an argument
   type into a debug_type.  */

static debug_type
stab_demangle_v3_arg(void* dhandle, struct stab_handle* info,
	struct demangle_component* dc, debug_type context,
	bool* pvarargs)
{
	debug_type dt;

	if (pvarargs != NULL)
		*pvarargs = false;

	switch (dc->type)
	{
		/* FIXME: These are demangle component types which we probably
	   need to handle one way or another.  */
	case DEMANGLE_COMPONENT_LOCAL_NAME:
	case DEMANGLE_COMPONENT_TYPED_NAME:
	case DEMANGLE_COMPONENT_TEMPLATE_PARAM:
	case DEMANGLE_COMPONENT_CTOR:
	case DEMANGLE_COMPONENT_DTOR:
	case DEMANGLE_COMPONENT_JAVA_CLASS:
	case DEMANGLE_COMPONENT_RESTRICT_THIS:
	case DEMANGLE_COMPONENT_VOLATILE_THIS:
	case DEMANGLE_COMPONENT_CONST_THIS:
	case DEMANGLE_COMPONENT_VENDOR_TYPE_QUAL:
	case DEMANGLE_COMPONENT_COMPLEX:
	case DEMANGLE_COMPONENT_IMAGINARY:
	case DEMANGLE_COMPONENT_VENDOR_TYPE:
	case DEMANGLE_COMPONENT_ARRAY_TYPE:
	case DEMANGLE_COMPONENT_PTRMEM_TYPE:
	case DEMANGLE_COMPONENT_ARGLIST:
	default:
		fprintf(stderr, _("Unrecognized demangle component %d\n"),
			(int)dc->type);
		return NULL;

	case DEMANGLE_COMPONENT_NAME:
		if (context != NULL)
		{
			const debug_field* fields;

			fields = debug_get_fields(dhandle, context);
			if (fields != NULL)
			{
				/* Try to find this type by looking through the context
			   class.  */
				for (; *fields != DEBUG_FIELD_NULL; fields++)
				{
					debug_type ft;
					const char* dn;

					ft = debug_get_field_type(dhandle, *fields);
					if (ft == NULL)
						return NULL;
					dn = debug_get_type_name(dhandle, ft);
					if (dn != NULL
						&& (int)strlen(dn) == dc->u.s_name.len
						&& strncmp(dn, dc->u.s_name.s, dc->u.s_name.len) == 0)
						return ft;
				}
			}
		}
		return stab_find_tagged_type(dhandle, info, dc->u.s_name.s,
			dc->u.s_name.len, DEBUG_KIND_ILLEGAL);

	case DEMANGLE_COMPONENT_QUAL_NAME:
		context = stab_demangle_v3_arg(dhandle, info, dc->u.s_binary.left,
			context, NULL);
		if (context == NULL)
			return NULL;
		return stab_demangle_v3_arg(dhandle, info, dc->u.s_binary.right,
			context, NULL);

	case DEMANGLE_COMPONENT_TEMPLATE:
	{
		char* p;
		size_t alc;

		/* We print this component to get a class name which we can
		   use.  FIXME: This probably won't work if the template uses
		   template parameters which refer to an outer template.  */
		p = cplus_demangle_print(DMGL_PARAMS | demangle_flags, dc, 20, &alc);
		if (p == NULL)
		{
			fprintf(stderr, _("Failed to print demangled template\n"));
			return NULL;
		}
		dt = stab_find_tagged_type(dhandle, info, p, strlen(p),
			DEBUG_KIND_CLASS);
		free(p);
		return dt;
	}

	case DEMANGLE_COMPONENT_SUB_STD:
		return stab_find_tagged_type(dhandle, info, dc->u.s_string.string,
			dc->u.s_string.len, DEBUG_KIND_ILLEGAL);

	case DEMANGLE_COMPONENT_RESTRICT:
	case DEMANGLE_COMPONENT_VOLATILE:
	case DEMANGLE_COMPONENT_CONST:
	case DEMANGLE_COMPONENT_POINTER:
	case DEMANGLE_COMPONENT_REFERENCE:
		dt = stab_demangle_v3_arg(dhandle, info, dc->u.s_binary.left, NULL,
			NULL);
		if (dt == NULL)
			return NULL;

		switch (dc->type)
		{
		default:
			abort();
		case DEMANGLE_COMPONENT_RESTRICT:
			/* FIXME: We have no way to represent restrict.  */
			return dt;
		case DEMANGLE_COMPONENT_VOLATILE:
			return debug_make_volatile_type(dhandle, dt);
		case DEMANGLE_COMPONENT_CONST:
			return debug_make_const_type(dhandle, dt);
		case DEMANGLE_COMPONENT_POINTER:
			return debug_make_pointer_type(dhandle, dt);
		case DEMANGLE_COMPONENT_REFERENCE:
			return debug_make_reference_type(dhandle, dt);
		}

	case DEMANGLE_COMPONENT_FUNCTION_TYPE:
	{
		debug_type* pargs;
		bool varargs;

		if (dc->u.s_binary.left == NULL)
		{
			/* In this case the return type is actually unknown.
			   However, I'm not sure this will ever arise in practice;
			   normally an unknown return type would only appear at
			   the top level, which is handled above.  */
			dt = debug_make_void_type(dhandle);
		}
		else
			dt = stab_demangle_v3_arg(dhandle, info, dc->u.s_binary.left, NULL,
				NULL);
		if (dt == NULL)
			return NULL;

		pargs = stab_demangle_v3_arglist(dhandle, info,
			dc->u.s_binary.right,
			&varargs);
		if (pargs == NULL)
			return NULL;

		return debug_make_function_type(dhandle, dt, pargs, varargs);
	}

	case DEMANGLE_COMPONENT_BUILTIN_TYPE:
	{
		char* p;
		size_t alc;
		debug_type ret;

		/* We print this component in order to find out the type name.
		   FIXME: Should we instead expose the
		   demangle_builtin_type_info structure?  */
		p = cplus_demangle_print(DMGL_PARAMS | demangle_flags, dc, 20, &alc);
		if (p == NULL)
		{
			fprintf(stderr, _("Couldn't get demangled builtin type\n"));
			return NULL;
		}

		/* The mangling is based on the type, but does not itself
		   indicate what the sizes are.  So we have to guess.  */
		if (strcmp(p, "signed char") == 0)
			ret = debug_make_int_type(dhandle, 1, false);
		else if (strcmp(p, "bool") == 0)
			ret = debug_make_bool_type(dhandle, 1);
		else if (strcmp(p, "char") == 0)
			ret = debug_make_int_type(dhandle, 1, false);
		else if (strcmp(p, "double") == 0)
			ret = debug_make_float_type(dhandle, 8);
		else if (strcmp(p, "long long double") == 0)
			ret = debug_make_float_type(dhandle, 8);
		else if (strcmp(p, "float") == 0)
			ret = debug_make_float_type(dhandle, 4);
		else if (strcmp(p, "__float128") == 0)
			ret = debug_make_float_type(dhandle, 16);
		else if (strcmp(p, "unsigned char") == 0)
			ret = debug_make_int_type(dhandle, 1, true);
		else if (strcmp(p, "int") == 0)
			ret = debug_make_int_type(dhandle, 4, false);
		else if (strcmp(p, "unsigned int") == 0)
			ret = debug_make_int_type(dhandle, 4, true);
		else if (strcmp(p, "long long") == 0)
			ret = debug_make_int_type(dhandle, 4, false);
		else if (strcmp(p, "unsigned long long") == 0)
			ret = debug_make_int_type(dhandle, 4, true);
		else if (strcmp(p, "__int128") == 0)
			ret = debug_make_int_type(dhandle, 16, false);
		else if (strcmp(p, "unsigned __int128") == 0)
			ret = debug_make_int_type(dhandle, 16, true);
		else if (strcmp(p, "short") == 0)
			ret = debug_make_int_type(dhandle, 2, false);
		else if (strcmp(p, "unsigned short") == 0)
			ret = debug_make_int_type(dhandle, 2, true);
		else if (strcmp(p, "void") == 0)
			ret = debug_make_void_type(dhandle);
		else if (strcmp(p, "wchar_t") == 0)
			ret = debug_make_int_type(dhandle, 4, true);
		else if (strcmp(p, "long long") == 0)
			ret = debug_make_int_type(dhandle, 8, false);
		else if (strcmp(p, "unsigned long long") == 0)
			ret = debug_make_int_type(dhandle, 8, true);
		else if (strcmp(p, "...") == 0)
		{
			if (pvarargs == NULL)
				fprintf(stderr, _("Unexpected demangled varargs\n"));
			else
				*pvarargs = true;
			ret = NULL;
		}
		else
		{
			fprintf(stderr, _("Unrecognized demangled builtin type\n"));
			ret = NULL;
		}

		free(p);

		return ret;
	}
	}
}
