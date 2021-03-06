/*
 * pg_hexedit.c - PostgreSQL file dump utility for
 *                viewing heap (data) and index files in wxHexEditor.
 *
 * Copyright (c) 2017, VMware, Inc.
 * Copyright (c) 2002-2010 Red Hat, Inc.
 * Copyright (c) 2011-2016, PostgreSQL Global Development Group
 *
 * This specialized fork of pg_filedump is modified to output XML that can be
 * used to annotate pages within the wxHexEditor hex editor.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Original pg_filedump Author: Patrick Macdonald <patrickm@redhat.com>
 * pg_hexedit author:           Peter Geoghegan <pg@bowt.ie>
 */
#define FRONTEND 1
#include "postgres.h"

/*
 * We must #undef frontend because certain headers are not really supposed to
 * be included in frontend utilities because they include atomics.h.
 */
#undef FRONTEND

#include <time.h>

#include "access/gin_private.h"
#include "access/gist.h"
#include "access/hash.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/nbtree.h"
#include "access/spgist_private.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#include "utils/pg_crc.h"

#define FD_VERSION				"11.0"	/* version ID of pg_hexedit */
#define FD_PG_VERSION			"PostgreSQL 11.x"	/* PG version it works
													 * with */
#define SEQUENCE_MAGIC			0x1717	/* PostgreSQL defined magic number */
#define EOF_ENCOUNTERED 		(-1)	/* Indicator for partial read */

#define COLOR_FONT_STANDARD		"#313739"

#define COLOR_BLACK				"#515A5A"
#define COLOR_BLUE_DARK			"#2980B9"
#define COLOR_BLUE_LIGHT		"#3498DB"
#define COLOR_BROWN				"#97333D"
#define COLOR_GREEN_BRIGHT		"#50E964"
#define COLOR_GREEN_DARK		"#16A085"
#define COLOR_GREEN_LIGHT		"#1ABC9C"
#define COLOR_MAROON			"#E96950"
#define COLOR_PINK				"#E949D1"
#define COLOR_RED_DARK			"#912C21"
#define COLOR_RED_LIGHT			"#E74C3C"
#define COLOR_WHITE				"#CCD1D1"
#define COLOR_YELLOW_DARK		"#F1C40F"
#define COLOR_YELLOW_LIGHT		"#E9E850"

typedef enum blockSwitches
{
	BLOCK_RANGE = 0x00000020,	/* -R: Specific block range to dump */
	BLOCK_CHECKSUMS = 0x00000040,	/* -k: verify block checksums */
	BLOCK_SKIP_LEAF = 0x00000080	/* -l: Skip leaf pages (use whole page tag) */
} blockSwitches;

typedef enum segmentSwitches
{
	SEGMENT_SIZE_FORCED = 0x00000001,	/* -s: Segment size forced */
	SEGMENT_NUMBER_FORCED = 0x00000002	/* -n: Segment number forced */
} segmentSwitches;

/* -R[start]:Block range start */
static int	blockStart = -1;

/* -R[end]:Block range end */
static int	blockEnd = -1;

/* Possible value types for the Special Section */
typedef enum specialSectionTypes
{
	SPEC_SECT_NONE,				/* No special section on block */
	SPEC_SECT_SEQUENCE,			/* Sequence info in special section */
	SPEC_SECT_INDEX_BTREE,		/* BTree index info in special section */
	SPEC_SECT_INDEX_HASH,		/* Hash index info in special section */
	SPEC_SECT_INDEX_GIST,		/* GIST index info in special section */
	SPEC_SECT_INDEX_GIN,		/* GIN index info in special section */
	SPEC_SECT_INDEX_SPGIST,		/* SP - GIST index info in special section */
	SPEC_SECT_ERROR_UNKNOWN,	/* Unknown error */
	SPEC_SECT_ERROR_BOUNDARY	/* Boundary error */
} specialSectionTypes;

static unsigned int specialType = SPEC_SECT_NONE;

/*
 * Possible return codes from option validation routine.
 *
 * pg_hexedit doesn't do much with them now but maybe in the future...
 */
typedef enum optionReturnCodes
{
	OPT_RC_VALID,				/* All options are valid */
	OPT_RC_INVALID,				/* Improper option string */
	OPT_RC_FILE,				/* File problems */
	OPT_RC_DUPLICATE,			/* Duplicate option encountered */
	OPT_RC_COPYRIGHT			/* Copyright should be displayed */
} optionReturnCodes;

/*
 * Simple macro to check for duplicate options and then set an option flag for
 * later consumption
 */
#define SET_OPTION(_x,_y,_z) if (_x & _y)				\
							   {						\
								 rc = OPT_RC_DUPLICATE; \
								 duplicateSwitch = _z;	\
							   }						\
							 else						\
							   _x |= _y;

/*
 * Global variables for ease of use mostly
 */

/* Segment-related options */
static unsigned int segmentOptions = 0;

/*	Options for Block formatting operations */
static unsigned int blockOptions = 0;

/* File to dump or format */
static FILE *fp = NULL;

/* File name for display */
static char *fileName = NULL;

/* Cache for current block */
static char *buffer = NULL;

/* Current block size */
static unsigned int blockSize = 0;

/* Current block in file */
static unsigned int currentBlock = 0;

/* Segment size in bytes */
static unsigned int segmentSize = RELSEG_SIZE * BLCKSZ;

/* Number of current segment */
static unsigned int segmentNumber = 0;

/* Current wxHexEditor output tag number */
static unsigned int tagNumber = 0;

/* Offset of current block (in bytes) */
static unsigned int pageOffset = 0;

/* Number of bytes to format */
static unsigned int bytesToFormat = 0;

/* Block version number */
static unsigned int blockVersion = 0;

/* Program exit code */
static int	exitCode = 0;

typedef enum formatChoice
{
	ITEM_HEAP = 0x00000001,		/* Blocks contain HeapTuple items */
	ITEM_INDEX = 0x00000002		/* Blocks contain IndexTuple items */
} formatChoice;

static void DisplayOptions(unsigned int validOptions);
static unsigned int GetSegmentNumberFromFileName(const char *fileName);
static unsigned int ConsumeOptions(int numOptions, char **options);
static int GetOptionValue(char *optionString);
static unsigned int GetBlockSize(void);
static unsigned int GetSpecialSectionType(Page page);
static char *GetHeapTupleHeaderFlags(HeapTupleHeader htup, bool isInfomask2);
static bool IsBtreeMetaPage(Page page);
static void EmitXmlDocHeader(int numOptions, char **options);
static void EmitXmlPage(BlockNumber blkno);
static void EmitXmlFooter(void);
static void EmitXmlTag(BlockNumber blkno, uint32 level, const char *name,
					   const char *color, uint32 relfileOff,
					   uint32 relfileOffEnd);
static void EmitXmlTupleTag(BlockNumber blkno, OffsetNumber offset,
							const char *name, const char *color,
							uint32 relfileOff,
							uint32 relfileOffEnd);
static void EmitXmlHeapTuple(BlockNumber blkno, OffsetNumber offset,
							 HeapTupleHeader htup, uint32 relfileOff,
							 unsigned int itemSize);
static void EmitXmlIndexTuple(BlockNumber blkno, OffsetNumber offset,
							  IndexTuple tuple, uint32 relfileOff);
static void EmitXmlItemId(BlockNumber blkno, OffsetNumber offset,
						  ItemId itemId, uint32 relfileOff,
						  const char *textFlags);
static int EmitXmlPageHeader(Page page, BlockNumber blkno,
							 uint32 level);
static void EmitXmlTuples(BlockNumber blkno, Page page);
static void EmitXmlSpecial(BlockNumber blkno, uint32 level);
static void EmitXmlFile(void);


/*	Send properly formed usage information to the user. */
static void
DisplayOptions(unsigned int validOptions)
{
	if (validOptions == OPT_RC_COPYRIGHT)
		printf
			("\npg_hexedit Version %s (for %s)"
			 "\nCopyright (c) 2002-2010 Red Hat, Inc."
			 "\nCopyright (c) 2011-2016, PostgreSQL Global Development Group\n",
			 FD_VERSION, FD_PG_VERSION);

	printf
		("\nUsage: pg_hexedit [-hkl] [-R startblock [endblock]] [-s segsize] [-n segnumber] file\n\n"
		 "Display formatted contents of a PostgreSQL heap/index/control file\n"
		 "Defaults are: relative addressing, range of the entire file, block\n"
		 "               size as listed on block 0 in the file\n\n"
		 "The following options are valid for heap and index files:\n"
		 "  -h  Display this information\n"
		 "  -k  Verify block checksums\n"
		 "  -l  Skip non-root B-Tree leaf pages\n"
		 "  -R  Display specific block ranges within the file (Blocks are\n"
		 "      indexed from 0)\n" "        [startblock]: block to start at\n"
		 "        [endblock]: block to end at\n"
		 "      A startblock without an endblock will format the single block\n"
		 "  -s  Force segment size to [segsize]\n"
		 "  -n  Force segment number to [segnumber]\n"
		 "\nReport bugs to <pg@bowt.ie>\n");
}

/*
 * Determine segment number by segment file name. For instance, if file
 * name is /path/to/xxxx.7 procedure returns 7. Default return value is 0.
 */
static unsigned int
GetSegmentNumberFromFileName(const char *fileName)
{
	int			segnumOffset = strlen(fileName) - 1;

	if (segnumOffset < 0)
		return 0;

	while (isdigit(fileName[segnumOffset]))
	{
		segnumOffset--;
		if (segnumOffset < 0)
			return 0;
	}

	if (fileName[segnumOffset] != '.')
		return 0;

	return atoi(&fileName[segnumOffset + 1]);
}

/*
 * Iterate through the provided options and set the option flags.  An error
 * will result in a positive rc and will force a display of the usage
 * information.  This routine returns enum option ReturnCode values.
 */
static unsigned int
ConsumeOptions(int numOptions, char **options)
{
	unsigned int rc = OPT_RC_VALID;
	unsigned int x;
	unsigned int optionStringLength;
	char	   *optionString;
	char		duplicateSwitch = 0x00;

	for (x = 1; x < numOptions; x++)
	{
		optionString = options[x];
		optionStringLength = strlen(optionString);

		/*
		 * Range is a special case where we have to consume the next 1 or 2
		 * parameters to mark the range start and end
		 */
		if ((optionStringLength == 2) && (strcmp(optionString, "-R") == 0))
		{
			int			range = 0;

			SET_OPTION(blockOptions, BLOCK_RANGE, 'R');
			/* Only accept the range option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* Make sure there are options after the range identifier */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing range start identifier.\n");
				exitCode = 1;
				break;
			}

			/*
			 * Mark that we have the range and advance the option to what
			 * should be the range start. Check the value of the next
			 * parameter.
			 */
			optionString = options[++x];
			if ((range = GetOptionValue(optionString)) < 0)
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid range start identifier <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}

			/* The default is to dump only one block */
			blockStart = blockEnd = (unsigned int) range;

			/*
			 * We have our range start marker, check if there is an end marker
			 * on the option line.  Assume that the last option is the file we
			 * are dumping, so check if there are options range start marker
			 * and the file.
			 */
			if (x <= (numOptions - 3))
			{
				if ((range = GetOptionValue(options[x + 1])) >= 0)
				{
					/* End range must be => start range */
					if (blockStart <= range)
					{
						blockEnd = (unsigned int) range;
						x++;
					}
					else
					{
						rc = OPT_RC_INVALID;
						printf("Error: Requested block range start <%d> is "
							   "greater than end <%d>.\n", blockStart, range);
						exitCode = 1;
						break;
					}
				}
			}
		}
		/* Check for the special case where the user forces a segment size. */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-s") == 0))
		{
			int			localSegmentSize;

			SET_OPTION(segmentOptions, SEGMENT_SIZE_FORCED, 's');
			/* Only accept the forced size option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -s is the segment size */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing segment size identifier.\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be forced segment size */
			optionString = options[++x];
			if ((localSegmentSize = GetOptionValue(optionString)) > 0)
				segmentSize = (unsigned int) localSegmentSize;
			else
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid segment size requested <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}
		}

		/*
		 * Check for the special case where the user forces a segment number
		 * instead of having the tool determine it by file name.
		 */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-n") == 0))
		{
			int			localSegmentNumber;

			SET_OPTION(segmentOptions, SEGMENT_NUMBER_FORCED, 'n');
			/* Only accept the forced segment number option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -n is the segment number */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing segment number identifier.\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be forced segment number */
			optionString = options[++x];
			if ((localSegmentNumber = GetOptionValue(optionString)) > 0)
				segmentNumber = (unsigned int) localSegmentNumber;
			else
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid segment number requested <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}
		}
		/* The last option MUST be the file name */
		else if (x == (numOptions - 1))
		{
			/* Check to see if this looks like an option string before opening */
			if (optionString[0] != '-')
			{
				fp = fopen(optionString, "rb");
				if (fp)
				{
					fileName = options[x];
					if (!(segmentOptions & SEGMENT_NUMBER_FORCED))
						segmentNumber = GetSegmentNumberFromFileName(fileName);
				}
				else
				{
					rc = OPT_RC_FILE;
					printf("Error: Could not open file <%s>.\n", optionString);
					exitCode = 1;
					break;
				}
			}
			else
			{
				/*
				 * Could be the case where the help flag is used without a
				 * filename. Otherwise, the last option isn't a file
				 */
				if (strcmp(optionString, "-h") == 0)
					rc = OPT_RC_COPYRIGHT;
				else
				{
					rc = OPT_RC_FILE;
					printf("Error: Missing file name to dump.\n");
					exitCode = 1;
				}
				break;
			}
		}
		else
		{
			unsigned int y;

			/* Option strings must start with '-' and contain switches */
			if (optionString[0] != '-')
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid option string <%s>.\n", optionString);
				exitCode = 1;
				break;
			}

			/*
			 * Iterate through the singular option string, throw out garbage,
			 * duplicates and set flags to be used in formatting
			 */
			for (y = 1; y < optionStringLength; y++)
			{
				switch (optionString[y])
				{
						/* Display the usage screen */
					case 'h':
						rc = OPT_RC_COPYRIGHT;
						break;

						/* Verify block checksums */
					case 'k':
						SET_OPTION(blockOptions, BLOCK_CHECKSUMS, 'k');
						break;

						/* Skip non-root leaf pages */
					case 'l':
						SET_OPTION(blockOptions, BLOCK_SKIP_LEAF, 'l');
						break;

					default:
						rc = OPT_RC_INVALID;
						printf("Error: Unknown option <%c>.\n", optionString[y]);
						exitCode = 1;
						break;
				}

				if (rc)
					break;
			}
		}
	}

	if (rc == OPT_RC_DUPLICATE)
	{
		printf("Error: Duplicate option listed <%c>.\n", duplicateSwitch);
		exitCode = 1;
	}

	return (rc);
}

/*
 * Given the index into the parameter list, convert and return the current
 * string to a number if possible
 */
static int
GetOptionValue(char *optionString)
{
	unsigned int x;
	int			value = -1;
	int			optionStringLength = strlen(optionString);

	/* Verify the next option looks like a number */
	for (x = 0; x < optionStringLength; x++)
	{
		if (!isdigit((int) optionString[x]))
			break;
	}

	/* Convert the string to a number if it looks good */
	if (x == optionStringLength)
		value = atoi(optionString);

	return value;
}

/*
 * Read the page header off of block 0 to determine the block size used in this
 * file.  Can be overridden using the -S option.  The returned value is the
 * block size of block 0 on disk.
 */
static unsigned int
GetBlockSize(void)
{
	unsigned int pageHeaderSize = sizeof(PageHeaderData);
	unsigned int localSize = 0;
	int			bytesRead = 0;
	char		localCache[pageHeaderSize];

	/* Read the first header off of block 0 to determine the block size */
	bytesRead = fread(&localCache, 1, pageHeaderSize, fp);
	rewind(fp);

	if (bytesRead == pageHeaderSize)
		localSize = (unsigned int) PageGetPageSize(&localCache);
	else
	{
		printf("Error: Unable to read full page header from block 0.\n"
			   "  ===> Read %u bytes\n", bytesRead);
		exitCode = 1;
	}

	return (localSize);
}

/*
 * Determine the contents of the special section on the block and	return this
 * enum value
 */
static unsigned int
GetSpecialSectionType(Page page)
{
	unsigned int rc;
	unsigned int specialOffset;
	unsigned int specialSize;
	unsigned int specialValue;
	PageHeader	pageHeader = (PageHeader) page;

	/*
	 * If this is not a partial header, check the validity of the  special
	 * section offset and contents
	 */
	if (bytesToFormat > sizeof(PageHeaderData))
	{
		specialOffset = (unsigned int) pageHeader->pd_special;

		/*
		 * Check that the special offset can remain on the block or the
		 * partial block
		 */
		if ((specialOffset == 0) ||
			(specialOffset > blockSize) || (specialOffset > bytesToFormat))
			rc = SPEC_SECT_ERROR_BOUNDARY;
		else
		{
			/* we may need to examine last 2 bytes of page to identify index */
			uint16	   *ptype = (uint16 *) (buffer + blockSize - sizeof(uint16));

			specialSize = blockSize - specialOffset;

			/*
			 * If there is a special section, use its size to guess its
			 * contents, checking the last 2 bytes of the page in cases that
			 * are ambiguous.  Note we don't attempt to dereference  the
			 * pointers without checking bytesToFormat == blockSize.
			 */
			if (specialSize == 0)
				rc = SPEC_SECT_NONE;
			else if (specialSize == MAXALIGN(sizeof(uint32)))
			{
				/*
				 * If MAXALIGN is 8, this could be either a sequence or
				 * SP-GiST or GIN.
				 */
				if (bytesToFormat == blockSize)
				{
					specialValue = *((int *) (buffer + specialOffset));
					if (specialValue == SEQUENCE_MAGIC)
						rc = SPEC_SECT_SEQUENCE;
					else if (specialSize == MAXALIGN(sizeof(SpGistPageOpaqueData)) &&
							 *ptype == SPGIST_PAGE_ID)
						rc = SPEC_SECT_INDEX_SPGIST;
					else if (specialSize == MAXALIGN(sizeof(GinPageOpaqueData)))
						rc = SPEC_SECT_INDEX_GIN;
					else
						rc = SPEC_SECT_ERROR_UNKNOWN;
				}
				else
					rc = SPEC_SECT_ERROR_UNKNOWN;
			}

			/*
			 * SP-GiST and GIN have same size special section, so check the
			 * page ID bytes first
			 */
			else if (specialSize == MAXALIGN(sizeof(SpGistPageOpaqueData)) &&
					 bytesToFormat == blockSize &&
					 *ptype == SPGIST_PAGE_ID)
				rc = SPEC_SECT_INDEX_SPGIST;
			else if (specialSize == MAXALIGN(sizeof(GinPageOpaqueData)))
				rc = SPEC_SECT_INDEX_GIN;
			else if (specialSize > 2 && bytesToFormat == blockSize)
			{
				/*
				 * As of 8.3, BTree, Hash, and GIST all have the same size
				 * special section, but the last two bytes of the section can
				 * be checked to determine what's what.
				 */
				if (*ptype <= MAX_BT_CYCLE_ID &&
					specialSize == MAXALIGN(sizeof(BTPageOpaqueData)))
					rc = SPEC_SECT_INDEX_BTREE;
				else if (*ptype == HASHO_PAGE_ID &&
						 specialSize == MAXALIGN(sizeof(HashPageOpaqueData)))
					rc = SPEC_SECT_INDEX_HASH;
				else if (*ptype == GIST_PAGE_ID &&
						 specialSize == MAXALIGN(sizeof(GISTPageOpaqueData)))
					rc = SPEC_SECT_INDEX_GIST;
				else
					rc = SPEC_SECT_ERROR_UNKNOWN;
			}
			else
				rc = SPEC_SECT_ERROR_UNKNOWN;
		}
	}
	else
		rc = SPEC_SECT_ERROR_UNKNOWN;

	return rc;
}

/*
 * Given Heap tuple header, return string buffer with t_infomask or t_infomask2
 * flags.
 *
 * Note:  Caller is responsible for free()ing returned buffer.
 */
static char *
GetHeapTupleHeaderFlags(HeapTupleHeader htup, bool isInfomask2)
{
	unsigned int	bitmapLength = 0;
	unsigned int	oidLength = 0;
	unsigned int	computedLength;
	unsigned int	localHoff;
	unsigned int	localBitOffset;
	char		   *flagString = NULL;

	flagString = malloc(512);

	if (!flagString)
	{
		printf("\nError: Unable to create buffer of size <512>.\n");
		exitCode = 1;
		exit(exitCode);
	}

	localHoff = htup->t_hoff;
	localBitOffset = offsetof(HeapTupleHeaderData, t_bits);

	/*
	 * Place readable versions of the tuple info mask into a buffer.
	 * Assume that the string can not expand beyond 512 bytes.
	 */
	flagString[0] = '\0';
	if (!isInfomask2)
	{
		strcat(flagString, "t_infomask ( ");

		if (htup->t_infomask & HEAP_HASNULL)
			strcat(flagString, "HEAP_HASNULL|");
		if (htup->t_infomask & HEAP_HASVARWIDTH)
			strcat(flagString, "HEAP_HASVARWIDTH|");
		if (htup->t_infomask & HEAP_HASEXTERNAL)
			strcat(flagString, "HEAP_HASEXTERNAL|");
		if (htup->t_infomask & HEAP_HASOID)
			strcat(flagString, "HEAP_HASOID|");
		if (htup->t_infomask & HEAP_XMAX_KEYSHR_LOCK)
			strcat(flagString, "HEAP_XMAX_KEYSHR_LOCK|");
		if (htup->t_infomask & HEAP_COMBOCID)
			strcat(flagString, "HEAP_COMBOCID|");
		if (htup->t_infomask & HEAP_XMAX_EXCL_LOCK)
			strcat(flagString, "HEAP_XMAX_EXCL_LOCK|");
		if (htup->t_infomask & HEAP_XMAX_LOCK_ONLY)
			strcat(flagString, "HEAP_XMAX_LOCK_ONLY|");
		if (htup->t_infomask & HEAP_XMIN_COMMITTED)
			strcat(flagString, "HEAP_XMIN_COMMITTED|");
		if (htup->t_infomask & HEAP_XMIN_INVALID)
			strcat(flagString, "HEAP_XMIN_INVALID|");
		if (htup->t_infomask & HEAP_XMAX_COMMITTED)
			strcat(flagString, "HEAP_XMAX_COMMITTED|");
		if (htup->t_infomask & HEAP_XMAX_INVALID)
			strcat(flagString, "HEAP_XMAX_INVALID|");
		if (htup->t_infomask & HEAP_XMAX_IS_MULTI)
			strcat(flagString, "HEAP_XMAX_IS_MULTI|");
		if (htup->t_infomask & HEAP_UPDATED)
			strcat(flagString, "HEAP_UPDATED|");
		if (htup->t_infomask & HEAP_MOVED_OFF)
			strcat(flagString, "HEAP_MOVED_OFF|");
		if (htup->t_infomask & HEAP_MOVED_IN)
			strcat(flagString, "HEAP_MOVED_IN|");

		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';
		strcat(flagString, " )");
	}
	else
	{
		strcat(flagString, "t_infomask2 ( ");

		if (htup->t_infomask2 & HEAP_KEYS_UPDATED)
			strcat(flagString, "HEAP_KEYS_UPDATED|");
		if (htup->t_infomask2 & HEAP_HOT_UPDATED)
			strcat(flagString, "HEAP_HOT_UPDATED|");
		if (htup->t_infomask2 & HEAP_ONLY_TUPLE)
			strcat(flagString, "HEAP_ONLY_TUPLE|");

		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';
		strcat(flagString, " )");
	}

	/*
	 * As t_bits is a variable length array, and may contain an Oid field,
	 * determine the length of the header proper as a sanity check.
	 */
	if (htup->t_infomask & HEAP_HASNULL)
		bitmapLength = BITMAPLEN(HeapTupleHeaderGetNatts(htup));
	else
		bitmapLength = 0;

	if (htup->t_infomask & HEAP_HASOID)
		oidLength += sizeof(Oid);

	computedLength =
		MAXALIGN(localBitOffset + bitmapLength + oidLength);

	/*
	 * Inform the user of a header size mismatch or dump the t_bits array
	 */
	if (computedLength != localHoff)
	{
		printf
			("  Error: Computed header length not equal to header size.\n"
			 "         Computed <%u>  Header: <%d>\n", computedLength,
			 localHoff);

		exitCode = 1;
	}

	return flagString;
}

/*	Check whether page is a btree meta page */
static bool
IsBtreeMetaPage(Page page)
{
	PageHeader	pageHeader = (PageHeader) page;

	if ((PageGetSpecialSize(page) == (MAXALIGN(sizeof(BTPageOpaqueData))))
		&& (bytesToFormat == blockSize))
	{
		BTPageOpaque btpo =
		(BTPageOpaque) ((char *) page + pageHeader->pd_special);

		/* Must check the cycleid to be sure it's really btree. */
		if ((btpo->btpo_cycleid <= MAX_BT_CYCLE_ID) &&
			(btpo->btpo_flags & BTP_META))
			return true;
	}

	return false;
}

/*
 * Display a header for the dump so we know the file name, the options and the
 * time the dump was taken
 */
static void
EmitXmlDocHeader(int numOptions, char **options)
{
	unsigned int x;
	char		optionBuffer[52] = "\0";
	char		timeStr[1000];

	/* Format time without newline */
	time_t		rightNow = time(NULL);
	struct tm  *localNow = localtime(&rightNow);

	strftime(timeStr, sizeof(timeStr), "%H:%M:%S %A, %B %d %Y", localNow);

	/*
	 * Iterate through the options and cache them. The maximum we can display
	 * is 50 option characters + spaces.
	 */
	for (x = 1; x < (numOptions - 1); x++)
	{
		if ((strlen(optionBuffer) + strlen(options[x])) > 50)
			break;
		strcat(optionBuffer, options[x]);
		strcat(optionBuffer, " ");
	}

	printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	printf("<!-- Dump created on: %s -->\n", timeStr);
	printf("<!-- Options used: %s -->\n", (strlen(optionBuffer)) ? optionBuffer : "None");
	printf("<wxHexEditor_XML_TAG>\n");
	printf("  <filename path=\"%s\">\n", fileName);
}

/*
 * For each block, dump out formatted header and content information
 */
static void
EmitXmlPage(BlockNumber blkno)
{
	Page		page = (Page) buffer;
	uint32		level = UINT_MAX;
	int			rc;

	pageOffset = blockSize * currentBlock;
	specialType = GetSpecialSectionType(page);

	/*
	 * Either dump out the entire block in hex + ACSII fashion, or interpret
	 * the data based on block structure
	 */
	if (specialType == SPEC_SECT_INDEX_BTREE)
	{
		BTPageOpaque btreeSection = (BTPageOpaque) PageGetSpecialPointer(page);

		/* Only B-Tree tags get a "level" */
		level = btreeSection->btpo.level;

		/*
		 * We optionally itemize leaf blocks as whole tags, in order to limit
		 * the size of tag files sharply.  Internal pages tend to be more
		 * interesting, for many reasons, at least when it comes to performance
		 * optimizations.  Still, always display the root page when it happens
		 * to be a leaf (before the first root page split).
		 */
		if ((btreeSection->btpo_flags & BTP_LEAF) &&
			!(btreeSection->btpo_flags & BTP_ROOT) &&
			(blockOptions & BLOCK_SKIP_LEAF))
		{
			EmitXmlTag(blkno, level, "leaf page", COLOR_GREEN_DARK,
					   pageOffset,
					   (pageOffset + BLCKSZ) - 1);
			rc = 0;
			return;
		}
	}

	/*
	 * Every block that we aren't skipping as an uninteresting leaf page will
	 * have header, items and possibly a special section tags created.  Beware
	 * of partial block reads, though.
	 */
	rc = EmitXmlPageHeader(page, blkno, level);

	/* If we didn't encounter a partial read in header, carry on...  */
	if (rc != EOF_ENCOUNTERED)
	{
		EmitXmlTuples(blkno, page);

		if (specialType != SPEC_SECT_NONE)
			EmitXmlSpecial(blkno, level);
	}
}

static void
EmitXmlFooter(void)
{
	printf("  </filename>\n");
	printf("</wxHexEditor_XML_TAG>\n");
}

/*
 * Emit a wxHexEditor tag for tuple data.
 *
 * Note: endOffset is an offset to the last byte whose range the tag covers, so
 * callers generally pass (relfileOff + length) - 1.  This is slightly less
 * verbose than getting callers to pass length.
 *
 * B-Tree index callers may optionally pass a "level"
 */
static void
EmitXmlTag(BlockNumber blkno, uint32 level, const char *name, const char *color,
		   uint32 relfileOff, uint32 relfileOffEnd)
{
	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	if (level != UINT_MAX)
		printf("      <tag_text>block %u (level %u) %s</tag_text>\n", blkno, level, name);
	else
		printf("      <tag_text>block %u %s</tag_text>\n", blkno, name);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");
}

/*
 * Emit a wxHexEditor tag for individual tuple or special area tag.
 *
 * Note: relfileOffEnd is an offset to the last byte whose range the tag
 * covers, so callers generally pass (relfileOff + length) - 1.  This is
 * slightly less verbose than getting callers to pass length.
 */
static void
EmitXmlTupleTag(BlockNumber blkno, OffsetNumber offset, const char *name,
				const char *color, uint32 relfileOff, uint32 relfileOffEnd)
{
	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	printf("      <tag_text>(%u,%u) %s</tag_text>\n", blkno, offset, name);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");
}

/*
 * Emit a wxHexEditor tag for entire heap tuple.
 *
 * Note: Caller passes itemSize from ItemId because that's the only place that
 * it's available from.
 */
static void
EmitXmlHeapTuple(BlockNumber blkno, OffsetNumber offset,
				 HeapTupleHeader htup, uint32 relfileOff,
				 unsigned int itemSize)
{
	char	   *flagString;
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;

	/*
	 * The choice of colors here is not completely arbitrary, or based on
	 * aesthetic preferences.  There is some attempt at analogy in the choice
	 * of colors.  For example, xmin and xmax are symmetric, and so are both
	 * COLOR_RED_LIGHT.
	 */
	relfileOffNext = relfileOff + sizeof(TransactionId);
	EmitXmlTupleTag(blkno, offset, "xmin", COLOR_RED_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(TransactionId);
	EmitXmlTupleTag(blkno, offset, "xmax", COLOR_RED_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;

	if (!(htup->t_infomask & HEAP_MOVED))
	{
		/*
		 * t_cid is COLOR_RED_DARK in order to signal that it's associated with
		 * though somewhat different to xmin and xmax.
		 */
		relfileOffNext += sizeof(CommandId);
		EmitXmlTupleTag(blkno, offset, "t_cid", COLOR_RED_DARK, relfileOff,
						relfileOffNext - 1);
	}
	else
	{
		/*
		 * This must be a rare case where pg_upgrade has been run, and we're
		 * left with a tuple with a t_xvac field instead of a t_cid field,
		 * because at some point old-style VACUUM FULL was run. (This would
		 * have had to have been on or before version 9.0, which has been out
		 * of support for some time.)
		 *
		 * Make it COLOR_PINK, so that it sticks out like a sore thumb.
		 */
		StaticAssertStmt(sizeof(CommandId) == sizeof(TransactionId),
						 "t_cid width must match t_xvac");
		relfileOffNext += sizeof(TransactionId);
		EmitXmlTupleTag(blkno, offset, "t_xvac", COLOR_PINK, relfileOff,
						relfileOffNext - 1);
	}

	/*
	 * Don't use ItemPointerData directly, to avoid having
	 * apparent mix in endianness in these fields.  Delineate
	 * which subfield is which by using multiple tags.
	 *
	 * The block component of each TID is COLOR_BLUE_LIGHT.  The same color is
	 * used for ItemIds, since both are physical pointers.  offsetNumber is a
	 * logical pointer, though, and so we make that COLOR_BLUE_DARK to slightly
	 * distinguish it.
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_ctid->bi_hi", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_ctid->bi_lo", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	/*
	 * Note: offsetNumber could be SpecTokenOffsetNumber, but we don't annotate
	 * that
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_ctid->offsetNumber", COLOR_BLUE_DARK,
					relfileOff, relfileOffNext - 1);

	flagString = GetHeapTupleHeaderFlags(htup, true);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_GREEN_LIGHT, relfileOff,
					relfileOffNext - 1);
	free(flagString);
	flagString = GetHeapTupleHeaderFlags(htup, false);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_GREEN_DARK, relfileOff,
					relfileOffNext - 1);
	free(flagString);

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, in line
	 * with general convention
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint8);
	EmitXmlTupleTag(blkno, offset, "t_hoff", COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);
	/*
	 * Whatever follows must be null bitmap
	 *
	 * Note that an Oid field will appear as the final 4 bytes of t_bits when
	 * (t_infomask & HEAP_HASOID).  This seems like the most faithful
	 * representation, because there is never any distinct field to t_hoff in
	 * the heap tuple struct: macros like HeapTupleHeaderGetOid() are built
	 * around working backwards from t_hoff knowing that the last sizeof(Oid)/4
	 * bytes must be an Oid when Oids are in use (just like this code).  Oid is
	 * a fixed size field that hides at the end of the variable sized t_bits
	 * array.
	 */
	relfileOff = relfileOffNext;
	relfileOffNext = relfileOffOrig + htup->t_hoff;
	EmitXmlTupleTag(blkno, offset, "t_bits", COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);

	/*
	 * Tuple contents (all attributes/columns) is slightly off-white, to
	 * suggest that we can't parse it due to not having access to catalog
	 * metadata, but consider it to be "payload", in constrast to the plain
	 * white area in the "hole" between the upper and lower sections of each
	 * page.
	 */
	relfileOff = relfileOffNext;
	EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE, relfileOff,
					(relfileOffOrig + itemSize) - 1);
}

/*
 * Emit a wxHexEditor tag for entire index tuple.
 *
 * Note: Caller does not need to pass itemSize from ItemId, because that's
 * redundant in the case of IndexTuples.
 */
static void
EmitXmlIndexTuple(BlockNumber blkno, OffsetNumber offset, IndexTuple tuple,
				  uint32 relfileOff)
{
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;

	/* TID style matches EmitXmlHeapTuple() */
	relfileOffNext = relfileOff + sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_tid->bi_hi", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_tid->bi_lo", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_tid->offsetNumber", COLOR_BLUE_DARK,
					relfileOff, relfileOffNext - 1);

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, which
	 * also matches EmitXmlHeapTuple()
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(unsigned short);
	EmitXmlTupleTag(blkno, offset, "t_info", COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);

	/* tuple contents -- "minus infinity" items have none */
	relfileOff = relfileOffNext;
	relfileOffNext = relfileOffOrig + IndexTupleSize(tuple);
	if (relfileOff < relfileOffNext)
		EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE,
						relfileOff, relfileOffNext - 1);
}

/*
 * Emit a wxHexEditor tag for an item pointer (ItemId).
 */
static void
EmitXmlItemId(BlockNumber blkno, OffsetNumber offset, ItemId itemId,
			  uint32 relfileOff, const char *textFlags)
{
	/* Interpret the content of each ItemId separately */
	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%lu</end_offset>\n", (relfileOff + sizeof(ItemIdData)) - 1);
	printf("      <tag_text>(%u,%d) lp_len: %u, lp_off: %u, lp_flags: %s </tag_text>\n",
		   blkno, offset, ItemIdGetLength(itemId), ItemIdGetOffset(itemId), textFlags);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>" COLOR_BLUE_LIGHT "</note_colour>\n");
	printf("    </TAG>\n");
}

/*
 * Dump out a formatted block header for the requested block
 *
 * Unlike with pg_filedump, this is also where ItemId entries are printed.
 * This is necessary to satisfy the tag number ordering requirement of
 * wxHexEditor.
 */
static int
EmitXmlPageHeader(Page page, BlockNumber blkno, uint32 level)
{
	int			rc = 0;
	unsigned int headerBytes;

	/*
	 * Only attempt to format the header if the entire header (minus the item
	 * array) is available
	 */
	if (bytesToFormat < offsetof(PageHeaderData, pd_linp[0]))
	{
		headerBytes = bytesToFormat;
		rc = EOF_ENCOUNTERED;
	}
	else
	{
		PageHeader	pageHeader = (PageHeader) page;
		int			maxOffset = PageGetMaxOffsetNumber(page);
		char		flagString[100];

		headerBytes = offsetof(PageHeaderData, pd_linp[0]);
		blockVersion = (unsigned int) PageGetPageLayoutVersion(page);

		/* We don't count itemidarray as header */
		if (maxOffset > 0)
		{
			unsigned int itemsLength = maxOffset * sizeof(ItemIdData);

			if (bytesToFormat < (headerBytes + itemsLength))
			{
				headerBytes = bytesToFormat;
				rc = EOF_ENCOUNTERED;
			}
		}

		/* Interpret the content of the header */
		EmitXmlTag(blkno, level, "LSN", COLOR_YELLOW_LIGHT,
				   pageOffset,
				   (pageOffset + sizeof(PageXLogRecPtr)) - 1);
		EmitXmlTag(blkno, level, "checksum", COLOR_GREEN_BRIGHT,
				   pageOffset + offsetof(PageHeaderData, pd_checksum),
				   (pageOffset + offsetof(PageHeaderData, pd_flags)) - 1);

		/* Generate generic page header flags */
		flagString[0] = '\0';
		strcat(flagString, "pd_flags - ");
		if (pageHeader->pd_flags & PD_HAS_FREE_LINES)
			strcat(flagString, "PD_HAS_FREE_LINES|");
		if (pageHeader->pd_flags & PD_PAGE_FULL)
			strcat(flagString, "PD_PAGE_FULL|");
		if (pageHeader->pd_flags & PD_ALL_VISIBLE)
			strcat(flagString, "PD_ALL_VISIBLE|");
		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';

		EmitXmlTag(blkno, level, flagString, COLOR_YELLOW_DARK,
				   pageOffset + offsetof(PageHeaderData, pd_flags),
				   (pageOffset + offsetof(PageHeaderData, pd_lower)) - 1);
		EmitXmlTag(blkno, level, "pd_lower", COLOR_MAROON,
				   pageOffset + offsetof(PageHeaderData, pd_lower),
				   (pageOffset + offsetof(PageHeaderData, pd_upper)) - 1);
		EmitXmlTag(blkno, level, "pd_upper", COLOR_MAROON,
				   pageOffset + offsetof(PageHeaderData, pd_upper),
				   (pageOffset + offsetof(PageHeaderData, pd_special)) - 1);
		EmitXmlTag(blkno, level, "pd_special", COLOR_GREEN_BRIGHT,
				   pageOffset + offsetof(PageHeaderData, pd_special),
				   (pageOffset + offsetof(PageHeaderData, pd_pagesize_version)) - 1);
		EmitXmlTag(blkno, level, "pd_pagesize_version", COLOR_BROWN,
				   pageOffset + offsetof(PageHeaderData, pd_pagesize_version),
				   (pageOffset + offsetof(PageHeaderData, pd_prune_xid)) - 1);
		EmitXmlTag(blkno, level, "pd_prune_xid", COLOR_RED_LIGHT,
				   pageOffset + offsetof(PageHeaderData, pd_prune_xid),
				   (pageOffset + offsetof(PageHeaderData, pd_linp[0])) - 1);

		if (IsBtreeMetaPage(page))
		{
			uint32	metaStartOffset = pageOffset + MAXALIGN(SizeOfPageHeaderData);

			/*
			 * If it's a btree meta page, produce tags for the contents of the
			 * meta block
			 */
			EmitXmlTag(blkno, level, "btm_magic", COLOR_PINK,
					   metaStartOffset + offsetof(BTMetaPageData, btm_magic),
					   (metaStartOffset + offsetof(BTMetaPageData, btm_version) - 1));
			EmitXmlTag(blkno, level, "btm_version", COLOR_PINK,
					   metaStartOffset + offsetof(BTMetaPageData, btm_version),
					   (metaStartOffset + offsetof(BTMetaPageData, btm_root) - 1));
			EmitXmlTag(blkno, level, "btm_root", COLOR_PINK,
					   metaStartOffset + offsetof(BTMetaPageData, btm_root),
					   (metaStartOffset + offsetof(BTMetaPageData, btm_level) - 1));
			EmitXmlTag(blkno, level, "btm_level", COLOR_PINK,
					   metaStartOffset + offsetof(BTMetaPageData, btm_level),
					   (metaStartOffset + offsetof(BTMetaPageData, btm_fastroot) - 1));
			EmitXmlTag(blkno, level, "btm_fastroot", COLOR_PINK,
					   metaStartOffset + offsetof(BTMetaPageData, btm_fastroot),
					   (metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel) - 1));
			EmitXmlTag(blkno, level, "btm_fastlevel", COLOR_PINK,
					   metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel),
					   (metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel) + sizeof(uint32) - 1));
			headerBytes += sizeof(BTMetaPageData);
		}
		else
		{
			OffsetNumber offset;

			/*
			 * It's either a non-meta index page, or a heap page.  Create tags
			 * for all ItemId entries/item pointers on page.
			 */
			for (offset = FirstOffsetNumber;
				 offset <= maxOffset;
				 offset = OffsetNumberNext(offset))
			{
				ItemId			itemId;
				unsigned int	itemFlags;
				char			textFlags[16];

				itemId = PageGetItemId(page, offset);

				itemFlags = (unsigned int) ItemIdGetFlags(itemId);

				switch (itemFlags)
				{
					case LP_UNUSED:
						strcpy(textFlags, "LP_UNUSED");
						break;
					case LP_NORMAL:
						strcpy(textFlags, "LP_NORMAL");
						break;
					case LP_REDIRECT:
						strcpy(textFlags, "LP_REDIRECT");
						break;
					case LP_DEAD:
						strcpy(textFlags, "LP_DEAD");
						break;
					default:
						/* shouldn't be possible */
						sprintf(textFlags, "0x%02x", itemFlags);
						break;
				}

				EmitXmlItemId(blkno, offset, itemId,
							  pageOffset + headerBytes + (sizeof(ItemIdData) * (offset - 1)),
							  textFlags);
			}
		}

		/*
		 * Eye the contents of the header and alert the user to possible
		 * problems
		 */
		if ((maxOffset < 0) ||
			(maxOffset > blockSize) ||
			(blockVersion != PG_PAGE_LAYOUT_VERSION) || /* only one we support */
			(pageHeader->pd_upper > blockSize) ||
			(pageHeader->pd_upper > pageHeader->pd_special) ||
			(pageHeader->pd_lower <
			 (sizeof(PageHeaderData) - sizeof(ItemIdData)))
			|| (pageHeader->pd_lower > blockSize)
			|| (pageHeader->pd_upper < pageHeader->pd_lower)
			|| (pageHeader->pd_special > blockSize))
		{
			printf(" Error: Invalid header information.\n\n");
			exitCode = 1;
		}

		/*
		 * Verify checksums if requested
		 */
		if (blockOptions & BLOCK_CHECKSUMS)
		{
			uint32		delta = (segmentSize / blockSize) * segmentNumber;
			uint16		calc_checksum = pg_checksum_page(page, delta + blkno);

			if (calc_checksum != pageHeader->pd_checksum)
			{
				printf(" Error: checksum failure: calculated 0x%04x.\n\n",
					   calc_checksum);
				exitCode = 1;
			}
		}
	}

	/*
	 * If we have reached the end of file while interpreting the header, give
	 * up
	 */
	if (rc == EOF_ENCOUNTERED)
	{
		printf
			(" Error: End of block encountered within the header."
			 " Bytes read: %4u.\n\n", bytesToFormat);
		exitCode = 1;
	}

	return rc;
}

/*
 * Emit formatted items that reside on this block
 */
static void
EmitXmlTuples(BlockNumber blkno, Page page)
{
	OffsetNumber offset;
	unsigned int itemSize;
	unsigned int itemOffset;
	ItemId		itemId;
	int			maxOffset = PageGetMaxOffsetNumber(page);

	/*
	 * If it's a btree meta page, the meta block is where items would normally
	 * be; don't print garbage.
	 */
	if (IsBtreeMetaPage(page))
		return;

	/*
	 * Loop through the items on the block.  Check if the block is empty and
	 * has a sensible item array listed before running through each item
	 */
	if (maxOffset == 0)
	{
		printf("Empty block - no items listed \n");
		exitCode = 1;
		exit(exitCode);
	}
	else if ((maxOffset < 0) || (maxOffset > blockSize))
	{
		printf("Error: Item index corrupt on block. Offset: <%d>\n",
			   maxOffset);
		exitCode = 1;
		exit(exitCode);
	}
	else
	{
		int			formatAs;

		/* Use the special section to determine the format style */
		switch (specialType)
		{
			case SPEC_SECT_INDEX_HASH:
			case SPEC_SECT_INDEX_GIST:
			case SPEC_SECT_INDEX_GIN:
			case SPEC_SECT_INDEX_SPGIST:
				exitCode = 1;
				exit(exitCode);
				break;
			case SPEC_SECT_INDEX_BTREE:
				formatAs = ITEM_INDEX;
				break;
			default:
				formatAs = ITEM_HEAP;
				break;
		}

		for (offset = FirstOffsetNumber;
			 offset <= maxOffset;
			 offset = OffsetNumberNext(offset))
		{
			itemId = PageGetItemId(page, offset);
			itemSize = (unsigned int) ItemIdGetLength(itemId);
			itemOffset = (unsigned int) ItemIdGetOffset(itemId);

			/*
			 * Make sure the item can physically fit on this block before
			 * formatting.  Since in a future pg version lp_len might be used
			 * for abbreviated keys in indexes, only insist on this for heap
			 * pages
			 */
			if (formatAs == ITEM_HEAP &&
				((itemOffset + itemSize > blockSize) ||
				 (itemOffset + itemSize > bytesToFormat)))
			{
				printf("  Error: Item contents extend beyond block.\n"
					   "         BlockSize<%d> Bytes Read<%d> Item Start<%d>.\n",
					   blockSize, bytesToFormat, itemOffset + itemSize);
				exitCode = 1;
				exit(exitCode);
			}

			if (formatAs == ITEM_HEAP)
			{
				HeapTupleHeader htup;

				if (itemSize != 0)
				{
					htup = (HeapTupleHeader) PageGetItem(page, itemId);

					EmitXmlHeapTuple(blkno, offset, htup,
									 pageOffset + itemOffset, itemSize);
				}
			}
			else if (formatAs == ITEM_INDEX)
			{
				IndexTuple	tuple;

				tuple = (IndexTuple) PageGetItem(page, itemId);

				EmitXmlIndexTuple(blkno, offset, tuple,
								  pageOffset + itemOffset);
			}
		}
	}
}

/*
 * On blocks that have special sections, print the contents according to
 * previously determined special section type
 */
static void
EmitXmlSpecial(BlockNumber blkno, uint32 level)
{
	PageHeader	pageHeader = (PageHeader) buffer;
	char		flagString[100] = "\0";
	unsigned int specialOffset = pageHeader->pd_special;

	switch (specialType)
	{
		case SPEC_SECT_ERROR_UNKNOWN:
		case SPEC_SECT_ERROR_BOUNDARY:
			printf(" Error: Invalid special section encountered.\n");
			exitCode = 1;
			break;

		case SPEC_SECT_INDEX_BTREE:
			{
				BTPageOpaque btreeSection = (BTPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "btpo_prev", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_prev),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_next)) - 1);
				EmitXmlTag(blkno, level, "btpo_next", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_next),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo)) - 1);
				EmitXmlTag(blkno, level, "btpo.level", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_flags)) - 1);

				/* Generate B-Tree special area flags */
				strcat(flagString, "btpo_flags - ");
				if (btreeSection->btpo_flags & BTP_LEAF)
					strcat(flagString, "BTP_LEAF|");
				if (btreeSection->btpo_flags & BTP_ROOT)
					strcat(flagString, "BTP_ROOT|");
				if (btreeSection->btpo_flags & BTP_DELETED)
					strcat(flagString, "BTP_DELETED|");
				if (btreeSection->btpo_flags & BTP_META)
					strcat(flagString, "BTP_META|");
				if (btreeSection->btpo_flags & BTP_HALF_DEAD)
					strcat(flagString, "BTP_HALF_DEAD|");
				if (btreeSection->btpo_flags & BTP_SPLIT_END)
					strcat(flagString, "BTP_SPLIT_END|");
				if (btreeSection->btpo_flags & BTP_HAS_GARBAGE)
					strcat(flagString, "BTP_HAS_GARBAGE|");
				if (btreeSection->btpo_flags & BTP_INCOMPLETE_SPLIT)
					strcat(flagString, "BTP_INCOMPLETE_SPLIT|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_flags),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid)) - 1);
				EmitXmlTag(blkno, level, "btpo_cycleid", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid) + sizeof(BTCycleId)) - 1);
			}
			break;

		case SPEC_SECT_SEQUENCE:
		case SPEC_SECT_INDEX_HASH:
		case SPEC_SECT_INDEX_GIST:
		case SPEC_SECT_INDEX_GIN:
		case SPEC_SECT_INDEX_SPGIST:
			printf(" Unsupported special section type. Type: <%u>.\n", specialType);
			exitCode = 1;
			break;
	}
}

/*
 * Control the dumping of the blocks within the file
 */
static void
EmitXmlFile(void)
{
	unsigned int initialRead = 1;
	unsigned int contentsToDump = 1;

	/*
	 * If the user requested a block range, seek to the correct position
	 * within the file for the start block.
	 */
	if (blockOptions & BLOCK_RANGE)
	{
		unsigned int position = blockSize * blockStart;

		if (fseek(fp, position, SEEK_SET) != 0)
		{
			printf("Error: Seek error encountered before requested "
				   "start block <%d>.\n", blockStart);
			contentsToDump = 0;
			exitCode = 1;
		}
		else
			currentBlock = blockStart;
	}

	/*
	 * Iterate through the blocks in the file until you reach the end or the
	 * requested range end
	 */
	while (contentsToDump)
	{
		bytesToFormat = fread(buffer, 1, blockSize, fp);

		if (bytesToFormat == 0)
		{
			/*
			 * fseek() won't pop an error if you seek passed eof.  The next
			 * subsequent read gets the error.
			 */
			if (initialRead)
				printf("Error: Premature end of file encountered.\n");

			contentsToDump = 0;
		}
		else
			EmitXmlPage(currentBlock);

		/* Check to see if we are at the end of the requested range. */
		if ((blockOptions & BLOCK_RANGE) &&
			(currentBlock >= blockEnd) && (contentsToDump))
		{
			contentsToDump = 0;
		}
		else
			currentBlock++;

		initialRead = 0;
	}

	EmitXmlFooter();
}

/*
 * Consume the options and iterate through the given file, formatting as
 * requested.
 */
int
main(int argv, char **argc)
{
	/* If there is a parameter list, validate the options */
	unsigned int validOptions;

	validOptions = (argv < 2) ? OPT_RC_COPYRIGHT : ConsumeOptions(argv, argc);

	/*
	 * Display valid options if no parameters are received or invalid options
	 * where encountered
	 */
	if (validOptions != OPT_RC_VALID)
		DisplayOptions(validOptions);
	else
	{
		EmitXmlDocHeader(argv, argc);
		blockSize = GetBlockSize();

		/*
		 * On a positive block size, allocate a local buffer to store the
		 * subsequent blocks
		 */
		if (blockSize > 0)
		{
			buffer = (char *) malloc(blockSize);
			if (buffer)
				EmitXmlFile();
			else
			{
				printf("\nError: Unable to create buffer of size <%d>.\n",
					   blockSize);
				exitCode = 1;
			}
		}
	}

	/* Close out the file and get rid of the allocated block buffer */
	if (fp)
		fclose(fp);

	if (buffer)
		free(buffer);

	exit(exitCode);
}
