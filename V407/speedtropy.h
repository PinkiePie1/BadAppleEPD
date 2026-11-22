/*
	***WARNING THIS ONE IS BACKWARDS*** THIS IS NOT STANDARD SPEEDTROPY


	Speedtropy - Range-coding compression, with a small decoding footprint

	specifically designed to be faster, and smaller than vpx coding, but
	at the cost of a very small amount of compression ratio loss. Designed
	to avoid need for norm table as well as complicated ingest.

	Overall, takes about half as much comptation and code space to decode.

	2026 Charles Lohr (cnlohr)

	I hereby place the code and algorithm herein, into the public domain.

	If public domain is not possible in your jurisdiction, then you may
	license this file under the MIT/x11 or NewBSD Licenses.

	No AI was used in any way in the creation of this library.

Usage:

#define SPEEDTROPY_WRITE
#define SPEEDTROPY_READ
//#define SPEEDTROPY_EMBEDDED // Special, see below.
#define SPEEDTROPY_IMPLEMENTATION

#include "speedtropy.h"

See provided `example.c`

*/

#ifndef _SPEEDTROPY_H
#define _SPEEDTROPY_H

#ifndef SPEEDTROPY_DECORATOR
#define SPEEDTROPY_DECORATOR static
#endif

#include <stdint.h>

#ifdef SPEEDTROPY_WRITE
typedef struct speedtropy_writer_t
{
	uint8_t * bitstream;
	size_t    length;
	size_t    size;
	uint32_t  range;
	uint32_t  low;
	int       error;
} speedtropy_writer;

SPEEDTROPY_DECORATOR void SpeedtropyWriterInit( speedtropy_writer * w, uint8_t * bitstream, size_t bitstreamSize );
SPEEDTROPY_DECORATOR int SpeedtropyWriterEmit( speedtropy_writer * w, int bit, int probability );
SPEEDTROPY_DECORATOR int SpeedtropyWriterFinish( speedtropy_writer * w );
#endif

#ifdef SPEEDTROPY_READ
typedef struct speedtropy_reader_t
{
	const uint8_t * bitstream;
	const uint8_t * bitstream_end;
	uint32_t  range;
	uint32_t  value;
} speedtropy_reader;

SPEEDTROPY_DECORATOR void SpeedtropyReaderInit( speedtropy_reader * r, const uint8_t * bitstream, size_t length );
SPEEDTROPY_DECORATOR int SpeedtropyReaderRead( speedtropy_reader * r, int prob );
#endif

#ifdef SPEEDTROPY_EMBEDDED

//	const uint8_t * speedtropy_bitstream;
//	uint32_t  speedtropy_range;
//	uint32_t  speedtropy_value;

SPEEDTROPY_DECORATOR void SpeedtropyEmbeddedReadInit( const uint8_t * bitstream );
SPEEDTROPY_DECORATOR int SpeedtropyEmbeddedRead( uint8_t prob );

#endif


#ifdef SPEEDTROPY_IMPLEMENTATION

#ifdef SPEEDTROPY_WRITE

SPEEDTROPY_DECORATOR void SpeedtropyWriterInit( speedtropy_writer * w, uint8_t * bitstream, size_t bitstreamSize )
{
	w->bitstream = bitstream;
	w->size = bitstreamSize;
	w->length = 0;
	w->low = 0;
	w->range = 0x1000000;
	w->error = 0;
}

// Internal function.
static int SpeedtropyWriterReleaseBits( speedtropy_writer * w )
{
	size_t carrypt = w->length;
	size_t nextlength = carrypt+1;
	if( nextlength >= w->size )
		w->error = 1;

	if( w->error )
		return -1;


	uint32_t low = w->low;
	unsigned carry = low>>24;
	while( carry )
	{
		if( carrypt == 0 || carrypt > w->length )
		{
			w->error = 1;
			return -1;
		}
		carrypt--;
		uint32_t val = w->bitstream[carrypt];
		val ++;
		w->bitstream[carrypt]++;
		// TODO: Rework to set carry instead of break.
		if( val < 0x100 )
			break;
	}
	uint8_t emit = (low)>>16;
	w->range <<= 8;
	w->low = (low - (emit<<16)) << 8;
	w->bitstream[w->length++] = emit;

	return 0;
}

SPEEDTROPY_DECORATOR int SpeedtropyWriterEmit( speedtropy_writer * w, int bit, int probability )
{
	// Absolutes do not alter state.
	if( probability <= 0 || probability >= 256 ) return 0;

	uint32_t range = w->range;
	uint32_t r8prob = (range>>8) * probability;
	if( bit )
	{
		w->low += r8prob;
		range -= r8prob;
	}
	else
	{
		range = r8prob;
	}

	w->range = range;
	if( range <= (0x10000) )
		return SpeedtropyWriterReleaseBits( w );

	return 0;
}

SPEEDTROPY_DECORATOR int SpeedtropyWriterFinish( speedtropy_writer * w )
{
	SpeedtropyWriterReleaseBits( w );
	SpeedtropyWriterReleaseBits( w );
	SpeedtropyWriterReleaseBits( w );
	if( w->error ) return -1;
	else return w->length;
}

#endif

#ifdef SPEEDTROPY_READ

SPEEDTROPY_DECORATOR void SpeedtropyReaderInit( speedtropy_reader * r, const uint8_t * bitstream, size_t length )
{
	r->bitstream = bitstream;
	r->bitstream_end = bitstream + length;
	r->value = 0;
	r->range = 1;
}

SPEEDTROPY_DECORATOR int SpeedtropyReaderRead( speedtropy_reader * r, int prob )
{
	// Optional: You could say:
	// if( prob == 0 ) return 1;
	// if( prob == 255 ) return 0;

	uint32_t range = r->range;

	while( range <= (0x10000) )
	{
		//printf( "BUMP: %08lx %08lx\n", range, r->value ); 
		const uint8_t * bs = r->bitstream;
		if( bs >= r->bitstream_end )
		{
			return -1;
			//range <<= 8;
			//r->value = (r->value << 8) | 0xff;
		}
		else
		{
			uint8_t data = *(bs++);
			r->bitstream = bs;
			range <<= 8;
			r->value = (r->value << 8) | data;
		}
	}

	uint32_t split = (range>>8) * prob;
	uint32_t value = r->value;

	int bit = value >= split;

	if( bit )
	{
		r->value = value - split;
		range -= split;
	}
	else
	{
		range = split;
	}
	r->range = range;

	return bit;
}

#endif

#ifdef SPEEDTROPY_EMBEDDED

//	const uint8_t * speedtropy_bitstream;
//	uint32_t  speedtropy_range;
//	uint32_t  speedtropy_value;

SPEEDTROPY_DECORATOR void SpeedtropyEmbeddedReadInit( const uint8_t * bitstream )
{
	speedtropy_bitstream = bitstream;
	speedtropy_value = 0;
	speedtropy_range = 1;
}

SPEEDTROPY_DECORATOR int SpeedtropyEmbeddedRead( uint8_t prob )
{
//	if( prob == 0 ) return 0;

	// Code 256 not possible on embedded version.
	//if( prob == 256 ) return 1;

	while( speedtropy_range <= (0x10000) )
	{
		// We do not check on the embedded version.
		//if( bs >= r->bitstream_end )
		//{
		//	return -1;
		//	//range <<= 8;
		//	//r->value = (r->value << 8) | 0xff;
		//}
		//else
		{
			uint8_t data = *(speedtropy_bitstream++);
			speedtropy_range <<= 8;
			speedtropy_value = (speedtropy_value << 8) | data;
		}
	}

	uint32_t split = (speedtropy_range>>8) * prob;
	int bit = speedtropy_value >= split;

	if( !bit )
		speedtropy_range = split;
	else
	{
		speedtropy_value -= split;
		speedtropy_range -= split;
	}

	return bit;
}

#endif

#endif

#endif

