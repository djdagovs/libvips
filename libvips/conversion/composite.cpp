/* composite an array of images with PDF operators
 *
 * 25/9/17
 * 	- from bandjoin.c
 */

/*

    This file is part of VIPS.

    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define VIPS_DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/debug.h>

#include "pconversion.h"

/* Maximum number of input images -- why not?
 */
#define MAX_INPUT_IMAGES (64)

/* Maximum number of image bands.
 */
#define MAX_BANDS (64)

/* Uncomment to disable the vector path ... handy for debugging. 
#undef HAVE_VECTOR_ARITH
 */

/**
 * VipsBlendMode:
 * VIPS_BLEND_MODE_CLEAR:
 * VIPS_BLEND_MODE_SOURCE:
 * VIPS_BLEND_MODE_OVER:
 * VIPS_BLEND_MODE_IN:
 * VIPS_BLEND_MODE_OUT:
 * VIPS_BLEND_MODE_ATOP:
 * VIPS_BLEND_MODE_DEST:
 * VIPS_BLEND_MODE_DEST_OVER:
 * VIPS_BLEND_MODE_DEST_IN:
 * VIPS_BLEND_MODE_DEST_OUT:
 * VIPS_BLEND_MODE_DEST_ATOP:
 * VIPS_BLEND_MODE_XOR:
 * VIPS_BLEND_MODE_ADD:
 * VIPS_BLEND_MODE_SATURATE:
 * VIPS_BLEND_MODE_MULTIPLY:
 * VIPS_BLEND_MODE_SCREEN:
 * VIPS_BLEND_MODE_OVERLAY:
 * VIPS_BLEND_MODE_DARKEN:
 * VIPS_BLEND_MODE_LIGHTEN:
 * VIPS_BLEND_MODE_COLOUR_DODGE:
 * VIPS_BLEND_MODE_COLOUR_BURN:
 * VIPS_BLEND_MODE_HARD_LIGHT:
 * VIPS_BLEND_MODE_SOFT_LIGHT:
 * VIPS_BLEND_MODE_DIFFERENCE:
 * VIPS_BLEND_MODE_EXCLUSION:
 *
 * The various Porter-Duff and PDF blend modes. See vips_composite(), 
 * for example.
 *
 * The Cairo docs have a nice explanation of all the blend modes:
 *
 * https://www.cairographics.org/operators
 */

/* We have a vector path with gcc's vector attr.
 */
#ifdef HAVE_VECTOR_ARITH
/* A vector of four floats.
 */
typedef float v4f __attribute__((vector_size(4 * sizeof(float))));
#endif /*HAVE_VECTOR_ARITH*/

typedef struct _VipsComposite {
	VipsConversion parent_instance;

	/* The input images.
	 */
	VipsArrayImage *in;

	/* For N input images, N - 1 blend modes.
	 */
	VipsArrayInt *mode;

	/* Compositing space. This defaults to RGB, or B_W if we only have
	 * G and GA inputs.
	 */
	VipsInterpretation compositing_space;

	/* Set if the input images have already been premultiplied.
	 */
	gboolean premultiplied;

	/* The number of inputs. This can be less than the number of images in
	 * @in.
	 */
	int n;

	/* The number of non-alpha bands we are blending.
	 */
	int bands;

	/* The maximum value for each band, set from the image interpretation.
	 * This is used to scale each band to 0 - 1.
	 */
	double max_band[MAX_BANDS + 1];

#ifdef HAVE_VECTOR_ARITH
	/* max_band as a vector, for the RGBA case.
	 */
	v4f max_band_vec;
#endif /*HAVE_VECTOR_ARITH*/

} VipsComposite;

typedef VipsConversionClass VipsCompositeClass;

/* We need C linkage for this.
 */
extern "C" {
G_DEFINE_TYPE( VipsComposite, vips_composite, VIPS_TYPE_CONVERSION );
}

/* For each of the supported interpretations, the maximum value of each band.
 */
static int
vips_composite_max_band( VipsComposite *composite, double *max_band )
{
	double max_alpha;
	int b;

	max_alpha = 255.0;
	if( composite->compositing_space == VIPS_INTERPRETATION_GREY16 ||
		composite->compositing_space == VIPS_INTERPRETATION_RGB16 )
		max_alpha = 65535.0;

	for( b = 0; b <= composite->bands; b++ )
		max_band[b] = max_alpha;

	switch( composite->compositing_space ) {
	case VIPS_INTERPRETATION_XYZ:
		max_band[0] = VIPS_D65_X0;
		max_band[1] = VIPS_D65_Y0;
		max_band[2] = VIPS_D65_Z0;
		break;

	case VIPS_INTERPRETATION_LAB:
		max_band[0] = 100;
		max_band[1] = 128;
		max_band[2] = 128;
		break;

	case VIPS_INTERPRETATION_LCH:
		max_band[0] = 100;
		max_band[1] = 128;
		max_band[2] = 360;
		break;

	case VIPS_INTERPRETATION_CMC:
		max_band[0] = 100;
		max_band[1] = 128;
		max_band[2] = 360;
		break;

	case VIPS_INTERPRETATION_scRGB:
		max_band[0] = 1;
		max_band[1] = 1;
		max_band[2] = 1;
		break;

	case VIPS_INTERPRETATION_sRGB:
		max_band[0] = 255;
		max_band[1] = 255;
		max_band[2] = 255;
		break;

	case VIPS_INTERPRETATION_HSV:
		max_band[0] = 255;
		max_band[1] = 255;
		max_band[2] = 255;
		break;

	case VIPS_INTERPRETATION_RGB16:
		max_band[0] = 65535;
		max_band[1] = 65535;
		max_band[2] = 65535;
		break;

	case VIPS_INTERPRETATION_GREY16:
		max_band[0] = 65535;
		break;

	case VIPS_INTERPRETATION_YXY:
		max_band[0] = 100;
		max_band[1] = 1;
		max_band[2] = 1;
		break;

	case VIPS_INTERPRETATION_B_W:
		max_band[0] = 256;
		break;

	default:
		return( -1 );
	}

	return( 0 );
}

/* Cairo naming conventions:
 *
 * aR	alpha of result
 * aA	alpha of source A	(the new pixel)
 * aB	alpha of source B	(the thing we accumulate)
 * xR	colour band of result
 * xA	colour band of source A
 * xB	colour band of source B
 */

/* A is the new pixel coming in, of any non-complex type T. 
 *
 * We must scale incoming pixels to 0 - 1 by dividing by the scale[] vector.
 *
 * If premultipled is not set, we premultiply incoming pixels before blending.
 *
 * B is the double pixel we are accumulating. 
 */
template <typename T>
static void
vips_composite_blend( VipsComposite *composite, 
	VipsBlendMode mode, double * restrict B, T * restrict p )
{
	const int bands = composite->bands;

	double A[MAX_BANDS + 1];
	double aA;
	double aB;
	double aR;
	double t1;
	double t2;
	double t3;
	double f[MAX_BANDS + 1];

	/* Load and scale the pixel to 0 - 1.
	 */
	for( int b = 0; b <= bands; b++ )
		A[b] = p[b] / composite->max_band[b];

	aA = A[bands];
	aB = B[bands];

	/* We may need to premultiply A.
	 */
	if( !composite->premultiplied )
		for( int b = 0; b < bands; b++ )
			A[b] *= aA;

	switch( mode ) {
	case VIPS_BLEND_MODE_CLEAR:
		aR = 0;
		for( int b = 0; b < bands; b++ )
			B[b] = 0;
		break;

	case VIPS_BLEND_MODE_SOURCE:
		aR = aA;
		for( int b = 0; b < bands; b++ )
			B[b] = A[b];
		break;

	case VIPS_BLEND_MODE_OVER:
		aR = aA + aB * (1 - aA);
		t1 = 1 - aA;
		for( int b = 0; b < bands; b++ )
			B[b] = A[b] + t1 * B[b];
		break;

	case VIPS_BLEND_MODE_IN:
		aR = aA * aB;
		for( int b = 0; b < bands; b++ )
			B[b] = A[b];
		break;

	case VIPS_BLEND_MODE_OUT:
		aR = aA * (1 - aB);
		for( int b = 0; b < bands; b++ )
			B[b] = A[b];
		break;

	case VIPS_BLEND_MODE_ATOP:
		aR = aB;
		t1 = 1 - aA;
		for( int b = 0; b < bands; b++ )
			B[b] = A[b] + t1 * B[b];
		break;

	case VIPS_BLEND_MODE_DEST:
		aR = aB;
		// B = B
		break;

	case VIPS_BLEND_MODE_DEST_OVER:
		aR = aB + aA * (1 - aB);
		t1 = 1 - aB;
		for( int b = 0; b < bands; b++ )
			B[b] = B[b] + t1 * A[b];
		break;

	case VIPS_BLEND_MODE_DEST_IN:
		aR = aA * aB;
		// B = B
		break;

	case VIPS_BLEND_MODE_DEST_OUT:
		aR = (1 - aA) * aB;
		// B = B
		break;

	case VIPS_BLEND_MODE_DEST_ATOP:
		aR = aA;
		t1 = 1 - aA;
		for( int b = 0; b < bands; b++ )
			B[b] = t1 * A[b] + B[b];
		break;

	case VIPS_BLEND_MODE_XOR:
		aR = aA + aB - 2 * aA * aB;
		t1 = 1 - aB;
		t2 = 1 - aA;
		for( int b = 0; b < bands; b++ )
			B[b] = t1 * A[b] + t2 * B[b];
		break;

	case VIPS_BLEND_MODE_ADD:
		aR = VIPS_MIN( 1, aA + aB );
		for( int b = 0; b < bands; b++ )
			B[b] = A[b] + B[b];
		break;

	case VIPS_BLEND_MODE_SATURATE:
		aR = VIPS_MIN( 1, aA + aB );
		t1 = VIPS_MIN( aA, 1 - aB );
		for( int b = 0; b < bands; b++ )
			B[b] = t1 * A[b] + B[b];
		break;

	default:
		/* The PDF modes are a bit different.
		 */
		aR = aA + aB * (1 - aA);

		switch( mode ) {
		case VIPS_BLEND_MODE_MULTIPLY:
			for( int b = 0; b < bands; b++ ) 
				f[b] = A[b] * B[b];
			break;

		case VIPS_BLEND_MODE_SCREEN:
			for( int b = 0; b < bands; b++ ) 
				f[b] = A[b] + B[b] - A[b] * B[b];
			break;

		case VIPS_BLEND_MODE_OVERLAY:
			for( int b = 0; b < bands; b++ ) 
				if( B[b] <= 0.5 ) 
					f[b] = 2 * A[b] * B[b];
				else 
					f[b] = 1 - 2 * (1 - A[b]) * (1 - B[b]);
			break;

		case VIPS_BLEND_MODE_DARKEN:
			for( int b = 0; b < bands; b++ ) 
				f[b] = VIPS_MIN( A[b], B[b] );
			break;

		case VIPS_BLEND_MODE_LIGHTEN:
			for( int b = 0; b < bands; b++ ) 
				f[b] = VIPS_MAX( A[b], B[b] );
			break;

		case VIPS_BLEND_MODE_COLOUR_DODGE:
			for( int b = 0; b < bands; b++ ) 
				if( A[b] < 1 ) 
					f[b] = VIPS_MIN( 1, B[b] / (1 - A[b]) );
				else 
					f[b] = 1;
			break;

		case VIPS_BLEND_MODE_COLOUR_BURN:
			for( int b = 0; b < bands; b++ ) 
				if( A[b] > 0 ) 
					f[b] = 1 - VIPS_MIN( 1, 
						(1 - B[b]) / A[b] );
				else 
					f[b] = 0;
			break;

		case VIPS_BLEND_MODE_HARD_LIGHT:
			for( int b = 0; b < bands; b++ ) 
				if( A[b] < 0.5 ) 
					f[b] = 2 * A[b] * B[b];
				else 
					f[b] = 1 - 2 * (1 - A[b]) * (1 - B[b]);
			break;

		case VIPS_BLEND_MODE_SOFT_LIGHT:
			for( int b = 0; b < bands; b++ ) {
				double g;

				if( B[b] <= 0.25 ) 
					g = ((16 * B[b] - 12) * B[b] + 4) * B[b];
				else 
					g = sqrt( B[b] );

				if( A[b] <= 0.5 )
					f[b] = B[b] - (1 - 2 * A[b]) * 
						B[b] * (1 - B[b]);
				else
					f[b] = B[b] + (2 * A[b] - 1) * 
						(g - B[b]);
			}
			break;

		case VIPS_BLEND_MODE_DIFFERENCE:
			for( int b = 0; b < bands; b++ ) 
				f[b] = abs( B[b] - A[b] );
			break;

		case VIPS_BLEND_MODE_EXCLUSION:
			for( int b = 0; b < bands; b++ ) 
				f[b] = A[b] + B[b] - 2 * A[b] * B[b];
			break;

		default:
			g_assert_not_reached();
			for( int b = 0; b < bands; b++ )
				B[b] = 0;
		}

		t1 = 1 - aB;
		t2 = 1 - aA;
		t3 = aA * aB;
		for( int b = 0; b < bands; b++ ) 
			B[b] = t1 * A[b] + t2 * B[b] + t3 * f[b];
		break;
	}

	B[bands] = aR;
}

/* We have a vector path with gcc's vector attr.
 */
#ifdef HAVE_VECTOR_ARITH
/* Special path for RGBA with non-double output. This is overwhelmingly the most 
 * common case, and vectorises easily. 
 *
 * B is the float pixel we are accumulating, A is the new pixel coming 
 * in from memory.
 */
template <typename T>
static void
vips_composite_blend3( VipsComposite *composite, 
	VipsBlendMode mode, v4f &B, T * restrict p )
{
	v4f A;
	float aA;
	float aB;
	float aR;
	float t1;
	float t2;
	float t3;
	v4f f;
	v4f g;

	/* Load and scale the pixel to 0 - 1.
	 */
	A[0] = p[0];
	A[1] = p[1];
	A[2] = p[2];
	A[3] = p[3];

	A /= composite->max_band_vec;

	aA = A[3];
	aB = B[3];

	/* We may need to premultiply A.
	 */
	if( !composite->premultiplied )
		A *= aA;

	switch( mode ) {
	case VIPS_BLEND_MODE_CLEAR:
		aR = 0;
		B[0] = 0;
		B[1] = 0;
		B[2] = 0;
		break;

	case VIPS_BLEND_MODE_SOURCE:
		aR = aA;
		B = A;
		break;

	case VIPS_BLEND_MODE_OVER:
		aR = aA + aB * (1 - aA);
		t1 = 1 - aA;
		B = A + t1 * B;
		break;

	case VIPS_BLEND_MODE_IN:
		aR = aA * aB;
		B = A;
		break;

	case VIPS_BLEND_MODE_OUT:
		aR = aA * (1 - aB);
		B = A;
		break;

	case VIPS_BLEND_MODE_ATOP:
		aR = aB;
		t1 = 1 - aA;
		B = A + t1 * B;
		break;

	case VIPS_BLEND_MODE_DEST:
		aR = aB;
		// B = B
		break;

	case VIPS_BLEND_MODE_DEST_OVER:
		aR = aB + aA * (1 - aB);
		t1 = 1 - aB;
		B = B + t1 * A;
		break;

	case VIPS_BLEND_MODE_DEST_IN:
		aR = aA * aB;
		// B = B
		break;

	case VIPS_BLEND_MODE_DEST_OUT:
		aR = (1 - aA) * aB;
		// B = B
		break;

	case VIPS_BLEND_MODE_DEST_ATOP:
		aR = aA;
		t1 = 1 - aA;
		B = t1 * A + B;
		break;

	case VIPS_BLEND_MODE_XOR:
		aR = aA + aB - 2 * aA * aB;
		t1 = 1 - aB;
		t2 = 1 - aA;
		B = t1 * A + t2 * B;
		break;

	case VIPS_BLEND_MODE_ADD:
		aR = VIPS_MIN( 1, aA + aB );
		B = A + B;
		break;

	case VIPS_BLEND_MODE_SATURATE:
		aR = VIPS_MIN( 1, aA + aB );
		t1 = VIPS_MIN( aA, 1 - aB );
		B = t1 * A + B;
		break;

	default:
		/* The PDF modes are a bit different.
		 */
		aR = aA + aB * (1 - aA);

		switch( mode ) {
		case VIPS_BLEND_MODE_MULTIPLY:
			f = A * B;
			break;

		case VIPS_BLEND_MODE_SCREEN:
			f = A + B - A * B;
			break;

		case VIPS_BLEND_MODE_OVERLAY:
			f = B <= 0.5 ? 
				2 * A * B :
				1 - 2 * (1 - A) * (1 - B);
			break;

		case VIPS_BLEND_MODE_DARKEN:
			f = VIPS_MIN( A, B );
			break;

		case VIPS_BLEND_MODE_LIGHTEN:
			f = VIPS_MAX( A, B );
			break;

		case VIPS_BLEND_MODE_COLOUR_DODGE:
			f = A < 1 ? 
				VIPS_MIN( 1, B / (1 - A) ) : 
				1;
			break;

		case VIPS_BLEND_MODE_COLOUR_BURN:
			f = A > 0 ? 
				1 - VIPS_MIN( 1, (1 - B) / A ) :
				0;
			break;

		case VIPS_BLEND_MODE_HARD_LIGHT:
			f = A < 0.5 ? 
				2 * A * B :
				1 - 2 * (1 - A) * (1 - B);
			break;

		case VIPS_BLEND_MODE_SOFT_LIGHT:
			/* You can't sqrt a vector, so we must loop.
			 */
			for( int b = 0; b < 3; b++ ) {
				double g;

				if( B[b] <= 0.25 ) 
					g = ((16 * B[b] - 12) * B[b] + 4) * B[b];
				else 
					g = sqrt( B[b] );

				if( A[b] <= 0.5 )
					f[b] = B[b] - (1 - 2 * A[b]) * 
						B[b] * (1 - B[b]);
				else
					f[b] = B[b] + (2 * A[b] - 1) * 
						(g - B[b]);
			}
			break;

		case VIPS_BLEND_MODE_DIFFERENCE:
			g = B - A;
			f = g > 0 ? g : -1 * g;
			break;

		case VIPS_BLEND_MODE_EXCLUSION:
			f = A + B - 2 * A * B;
			break;

		default:
			g_assert_not_reached();
			for( int b = 0; b < 3; b++ ) 
				B[b] = 0;
		}

		t1 = 1 - aB;
		t2 = 1 - aA;
		t3 = aA * aB;
		B = t1 * A + t2 * B + t3 * f;
		break;
	}

	B[3] = aR;
}
#endif /*HAVE_VECTOR_ARITH*/

/* min_T and max_T are the numeric range for this type. 0, 0 means no limit,
 * for example float.
 */
template <typename T, gint64 min_T, gint64 max_T>
static void 
vips_combine_pixels( VipsComposite *composite, VipsPel *q, VipsPel **p )
{
	VipsBlendMode *m = (VipsBlendMode *) composite->mode->area.data;
	int n = composite->n;
	int bands = composite->bands;
	T * restrict tq = (T * restrict) q;
	T ** restrict tp = (T ** restrict) p;

	double B[MAX_BANDS + 1];
	double aB;

	/* Load and scale the base pixel to 0 - 1.
	 */
	for( int b = 0; b <= bands; b++ )
		B[b] = tp[0][b] / composite->max_band[b];

	aB = B[bands];
	if( !composite->premultiplied )
		for( int b = 0; b < bands; b++ )
			B[b] *= aB;

	for( int i = 1; i < n; i++ ) 
		vips_composite_blend<T>( composite, m[i - 1], B, tp[i] ); 

	/* Unpremultiply, if necessary.
	 */
	if( !composite->premultiplied ) {
		double aR = B[bands];

		if( aR == 0 )
			for( int b = 0; b < bands; b++ )
				B[b] = 0;
		else
			for( int b = 0; b < bands; b++ )
				B[b] = B[b] / aR;
	}

	/* Write back as a full range pixel, clipping to range.
	 */
	for( int b = 0; b <= bands; b++ ) {
		double v;

		v = B[b] * composite->max_band[b];
		if( min_T != 0 || 
			max_T != 0 ) {
			v = VIPS_CLIP( min_T, v, max_T ); 
		}

		tq[b] = v;
	}
}

#ifdef HAVE_VECTOR_ARITH
/* Three band (four with alpha) vecvtior case. Non-double output. min_T and 
 * max_T are the numeric range for this type. 0, 0 means no limit,
 * for example float.
 */
template <typename T, gint64 min_T, gint64 max_T>
static void 
vips_combine_pixels3( VipsComposite *composite, VipsPel *q, VipsPel **p )
{
	VipsBlendMode *m = (VipsBlendMode *) composite->mode->area.data;
	int n = composite->n;
	T * restrict tq = (T * restrict) q;
	T ** restrict tp = (T ** restrict) p;

	v4f B;
	float aB;

	B[0] = tp[0][0];
	B[1] = tp[0][1];
	B[2] = tp[0][2];
	B[3] = tp[0][3];

	/* Scale the base pixel to 0 - 1.
	 */
	B /= composite->max_band_vec;
	aB = B[3];

	if( !composite->premultiplied ) {
		B *= aB;
		B[3] = aB;
	}

	for( int i = 1; i < n; i++ ) 
		vips_composite_blend3<T>( composite, m[i - 1], B, tp[i] ); 

	/* Unpremultiply, if necessary.
	 */
	if( !composite->premultiplied ) {
		float aR = B[3];

		if( aR == 0 )
			for( int b = 0; b < 3; b++ ) 
				B[b] = 0;
		else {
			B /= aR;
			B[3] = aR;
		}
	}

	/* Write back as a full range pixel, clipping to range.
	 */
	B *= composite->max_band_vec;
	if( min_T != 0 || 
		max_T != 0 ) {
		float low = min_T;
		float high = max_T;

		B = VIPS_CLIP( low, B, high );
	}

	tq[0] = B[0];
	tq[1] = B[1];
	tq[2] = B[2];
	tq[3] = B[3];
}
#endif /*HAVE_VECTOR_ARITH*/

static int
vips_composite_gen( VipsRegion *output_region,
	void *seq, void *a, void *b, gboolean *stop )
{
	VipsRegion **input_regions = (VipsRegion **) seq;
	VipsComposite *composite = (VipsComposite *) b;
	VipsRect *r = &output_region->valid;
	int ps = VIPS_IMAGE_SIZEOF_PEL( output_region->im );

	if( vips_reorder_prepare_many( output_region->im, input_regions, r ) )
		return( -1 );

	VIPS_GATE_START( "vips_composite_gen: work" );

	for( int y = 0; y < r->height; y++ ) {
		VipsPel *p[MAX_INPUT_IMAGES];
		VipsPel *q;

		for( int i = 0; i < composite->n; i++ )
			p[i] = VIPS_REGION_ADDR( input_regions[i],
				r->left, r->top + y );
		p[composite->n] = NULL;
		q = VIPS_REGION_ADDR( output_region, r->left, r->top + y );

		for( int x = 0; x < r->width; x++ ) {
			switch( input_regions[0]->im->BandFmt ) {
			case VIPS_FORMAT_UCHAR: 	
#ifdef HAVE_VECTOR_ARITH
				if( composite->bands == 3 ) 
					vips_combine_pixels3
						<unsigned char, 0, UCHAR_MAX>
						( composite, q, p );
				else
#endif 
					vips_combine_pixels
						<unsigned char, 0, UCHAR_MAX>
						( composite, q, p );
				break;

			case VIPS_FORMAT_CHAR: 		
				vips_combine_pixels
					<signed char, SCHAR_MIN, SCHAR_MAX>
					( composite, q, p );
				break; 

			case VIPS_FORMAT_USHORT: 	
#ifdef HAVE_VECTOR_ARITH
				if( composite->bands == 3 ) 
					vips_combine_pixels3
						<unsigned short, 0, USHRT_MAX>
						( composite, q, p );
				else
#endif 
					vips_combine_pixels
						<unsigned short, 0, USHRT_MAX>
						( composite, q, p );
				break; 

			case VIPS_FORMAT_SHORT: 	
				vips_combine_pixels
					<signed short, SHRT_MIN, SHRT_MAX>
					( composite, q, p );
				break; 

			case VIPS_FORMAT_UINT: 		
				vips_combine_pixels
					<unsigned int, 0, UINT_MAX>
					( composite, q, p );
				break; 

			case VIPS_FORMAT_INT: 		
				vips_combine_pixels
					<signed int, INT_MIN, INT_MAX>
					( composite, q, p );
				break; 

			case VIPS_FORMAT_FLOAT:
#ifdef HAVE_VECTOR_ARITH
				if( composite->bands == 3 ) 
					vips_combine_pixels3
						<float, 0, USHRT_MAX>
						( composite, q, p );
				else
#endif 
					vips_combine_pixels
						<float, 0, 0>
						( composite, q, p );
				break;

			case VIPS_FORMAT_DOUBLE:
				vips_combine_pixels
					<double, 0, 0>
					( composite, q, p );
				break;

			default:
				g_assert_not_reached();
				return( -1 );
			}

			for( int i = 0; i < composite->n; i++ )
				p[i] += ps;
			q += ps;
		}
	}

	VIPS_GATE_STOP( "vips_composite_gen: work" );

	return( 0 );
}

static int
vips_composite_build( VipsObject *object )
{
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS( object );
	VipsConversion *conversion = VIPS_CONVERSION( object );
	VipsComposite *composite = (VipsComposite *) object;

	VipsImage **in;
	VipsImage **decode;
	VipsImage **compositing;
	VipsImage **format;
	VipsImage **size;
	VipsBlendMode *mode;

	if( VIPS_OBJECT_CLASS( vips_composite_parent_class )->build( object ) )
		return( -1 );

	composite->n = composite->in->area.n;

	if( composite->n <= 0 ) {
		vips_error( klass->nickname, "%s", _( "no input images" ) );
		return( -1 );
	}
	if( composite->mode->area.n != composite->n - 1 ) {
		vips_error( klass->nickname,
			_( "for %d input images there must be %d blend modes" ),
			composite->n, composite->n - 1 );
		return( -1 );
	}
	mode = (VipsBlendMode *) composite->mode->area.data;
	for( int i = 0; i < composite->n - 1; i++ ) {
		if( mode[i] < 0 ||
			mode[i] >= VIPS_BLEND_MODE_LAST ) {
			vips_error( klass->nickname,
				_( "blend mode index %d (%d) invalid" ),
				i, mode[i] );
			return( -1 );
		}
	}

	in = (VipsImage **) composite->in->area.data;

	decode = (VipsImage **) vips_object_local_array( object, composite->n );
	for( int i = 0; i < composite->n; i++ )
		if( vips_image_decode( in[i], &decode[i] ) )
			return( -1 );
	in = decode;

	/* Are any of the images missing an alpha? The first missing alpha is
	 * given a solid 255 and becomes the background image, shortening n.
	 */
	for( int i = composite->n - 1; i >= 0; i-- )
		if( !vips_image_hasalpha( in[i] ) ) {
			VipsImage *x;

			if( vips_addalpha( in[i], &x, NULL ) )
				return( -1 );
			g_object_unref( in[i] );
			in[i] = x;

			composite->n -= i;
			in += i;

			break;
		}

	if( composite->n > MAX_INPUT_IMAGES ) {
		vips_error( klass->nickname,
			"%s", _( "too many input images" ) );
		return( -1 );
	}

	/* Transform to compositing space. It defaults to sRGB or B_W, usually 
	 * 8 bit, but 16 bit if any inputs are 16 bit.
	 */
	if( !vips_object_argument_isset( object, "compositing_space" ) ) {
		gboolean all_grey;
		gboolean any_16;

		all_grey = TRUE;
		for( int i = 0; i < composite->n; i++ )
			if( in[i]->Bands > 2 ) {
				all_grey = FALSE;
				break;
			}

		any_16 = FALSE;
		for( int i = 0; i < composite->n; i++ )
			if( in[i]->Type == VIPS_INTERPRETATION_GREY16 ||
				in[i]->Type == VIPS_INTERPRETATION_RGB16 ) {
				any_16 = TRUE;
				break;
			}

		composite->compositing_space = any_16 ?
			(all_grey ?
			 VIPS_INTERPRETATION_GREY16 : 
			 VIPS_INTERPRETATION_RGB16) :
			(all_grey ?
			 VIPS_INTERPRETATION_B_W : 
			 VIPS_INTERPRETATION_sRGB);
	}

	compositing = (VipsImage **)
		vips_object_local_array( object, composite->n );
	for( int i = 0; i < composite->n; i++ )
		if( vips_colourspace( in[i], &compositing[i],
			composite->compositing_space, NULL ) )
			return( -1 );
	in = compositing;

	/* Check that they all now match in bands. This can fail for some
	 * input combinations.
	 */
	for( int i = 1; i < composite->n; i++ )
		if( in[i]->Bands != in[0]->Bands ) {
			vips_error( klass->nickname, 
				"%s", _( "images do not have same "
					 "numbers of bands" ) );
			return( -1 );
		}

	if( in[0]->Bands > MAX_BANDS ) {
		vips_error( klass->nickname,
			"%s", _( "too many input bands" ) );
		return( -1 );
	}

	composite->bands = in[0]->Bands - 1;

	/* Set the max for each band now we know bands and compositing space.
	 */
	if( vips_composite_max_band( composite, composite->max_band ) ) {
		vips_error( klass->nickname, 
			"%s", _( "unsupported compositing space" ) );
		return( -1 ); 
	}

#ifdef HAVE_VECTOR_ARITH
	/* We need a float version for the vector path.
	 */
	if( composite->bands == 3 ) 
		for( int b = 0; b <= 3; b++ )
			composite->max_band_vec[b] = composite->max_band[b];
#endif /*HAVE_VECTOR_ARITH*/

	/* Transform the input images to match in size and format. We may have
	 * mixed float and double, for example.  
	 */
	format = (VipsImage **) vips_object_local_array( object, composite->n );
	size = (VipsImage **) vips_object_local_array( object, composite->n );
	if( vips__formatalike_vec( in, format, composite->n ) ||
		vips__sizealike_vec( format, size, composite->n ) )
		return( -1 );
	in = size;

	if( vips_image_pipeline_array( conversion->out,
		VIPS_DEMAND_STYLE_THINSTRIP, in ) )
		return( -1 );

	if( vips_image_generate( conversion->out,
		vips_start_many, vips_composite_gen, vips_stop_many,
		in, composite ) )
		return( -1 );

	return( 0 );
}

static void
vips_composite_class_init( VipsCompositeClass *klass )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( klass );
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS( klass );
	VipsOperationClass *operation_class = VIPS_OPERATION_CLASS( klass );

	VIPS_DEBUG_MSG( "vips_composite_class_init\n" );

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "composite";
	vobject_class->description =
		_( "blend an array of images with an array of blend modes" );
	vobject_class->build = vips_composite_build;

	operation_class->flags = VIPS_OPERATION_SEQUENTIAL;

	VIPS_ARG_BOXED( klass, "in", 0,
		_( "Inputs" ),
		_( "Array of input images" ),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET( VipsComposite, in ),
		VIPS_TYPE_ARRAY_IMAGE );

	VIPS_ARG_BOXED( klass, "mode", 3,
		_( "Blend modes" ),
		_( "Array of VipsBlendMode to join with" ),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET( VipsComposite, mode ),
		VIPS_TYPE_ARRAY_INT );

	VIPS_ARG_ENUM( klass, "compositing_space", 10,
		_( "Compositing space" ),
		_( "Composite images in this colour space" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsComposite, compositing_space ),
		VIPS_TYPE_INTERPRETATION, VIPS_INTERPRETATION_sRGB );

	VIPS_ARG_BOOL( klass, "premultiplied", 11,
		_( "Premultiplied" ),
		_( "Images have premultiplied alpha" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsComposite, premultiplied ),
		FALSE );

}

static void
vips_composite_init( VipsComposite *composite )
{
	composite->compositing_space = VIPS_INTERPRETATION_sRGB;
}

static int
vips_compositev( VipsImage **in, VipsImage **out, int n, int *mode, va_list ap )
{
	VipsArrayImage *image_array;
	VipsArrayInt *mode_array;
	int result;

	image_array = vips_array_image_new( in, n );
	mode_array = vips_array_int_new( mode, n - 1 );
	result = vips_call_split( "composite", ap,
		image_array, out, mode_array );
	vips_area_unref( VIPS_AREA( image_array ) );
	vips_area_unref( VIPS_AREA( mode_array ) );

	return( result );
}

/**
 * vips_composite: (method)
 * @in: (array length=n) (transfer none): array of input images
 * @out: (out): output image
 * @n: number of input images
 * @mode: array of (@n - 1) #VipsBlendMode
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @compositing_space: #VipsInterpretation to composite in
 * * @premultiplied: %gboolean, images are already premultiplied
 *
 * Composite an array of images together. 
 *
 * Images are placed in a stack, with @in[0] at the bottom and @in[@n - 1] at
 * the top. Pixels are blended together working from the bottom upwards, with 
 * the blend mode at each step being set by the corresponding #VipsBlendMode
 * in @mode.
 *
 * Images are transformed to a compositing space before processing. This is
 * #VIPS_INTERPRETATION_sRGB, #VIPS_INTERPRETATION_B_W,
 * #VIPS_INTERPRETATION_RGB16, or #VIPS_INTERPRETATION_GREY16 
 * by default, depending on 
 * how many bands and bits the input images have. You can select any other 
 * space, such as #VIPS_INTERPRETATION_LAB or #VIPS_INTERPRETATION_scRGB.
 *
 * The output image is in the compositing space. It will always be 
 * #VIPS_FORMAT_FLOAT unless one of the inputs is #VIPS_FORMAT_DOUBLE, in 
 * which case the output will be double as well.
 *
 * Complex images are not supported.
 *
 * The output image will always have an alpha band. A solid alpha is
 * added to any input missing an alpha. 
 *
 * The images do not need to match in size or format. They will be expanded to
 * the smallest common size and format in the usual way.
 *
 * Image are normally treated as unpremultiplied, so this operation can be used
 * directly on PNG images. If your images have been through vips_premultiply(),
 * set @premultiplied. 
 *
 * See also: vips_insert().
 *
 * Returns: 0 on success, -1 on error
 */
int
vips_composite( VipsImage **in, VipsImage **out, int n, int *mode, ... )
{
	va_list ap;
	int result;

	va_start( ap, mode );
	result = vips_compositev( in, out, n, mode, ap );
	va_end( ap );

	return( result );
}

/**
 * vips_composite2: (method)
 * @base: first input image
 * @overlay: second input image
 * @out: (out): output image
 * @mode: composite with this blend mode
 * @...: %NULL-terminated list of optional named arguments
 *
 * Composite @overlay on top of @base with @mode. See vips_composite().
 *
 * Returns: 0 on success, -1 on error
 */
int
vips_composite2( VipsImage *base, VipsImage *overlay, VipsImage **out,
	VipsBlendMode mode, ... )
{
	va_list ap;
	int result;
	VipsImage *imagev[2];
	int modev[1];

	imagev[0] = base;
	imagev[1] = overlay;
	modev[0] = mode;

	va_start( ap, mode );
	result = vips_compositev( imagev, out, 2, modev, ap );
	va_end( ap );

	return( result );
}
