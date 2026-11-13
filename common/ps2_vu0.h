/* VU0 in macro mode (COP2 instructions inline on the EE) for small 4-wide math.
 * Shared by the engine and the game libraries. Every helper needs 16-byte aligned
 * rows or quaternions and returns 0 when a pointer is not aligned, so the caller
 * keeps its scalar code for that case. -DPS2_NO_VU0 turns them all off. */
#ifndef PS2_VU0_H
#define PS2_VU0_H

#if defined( __PS2__ ) && !defined( PS2_NO_VU0 )

#define PS2_VU0 1
#define PS2_ALIGN16 __attribute__(( aligned( 16 )))
#define PS2_VU0_ALIGNED( a ) ((( unsigned int )( a ) & 15u ) == 0 )

/* out = in1 * in2 for 3x4 matrices, as ConcatTransforms. All inputs are loaded
 * before the first store, so out may be one of the inputs. */
static inline int PS2_VU0_ConcatTransforms( const float in1[3][4], const float in2[3][4], float out[3][4] )
{
	if( !PS2_VU0_ALIGNED( (unsigned int)in1 | (unsigned int)in2 | (unsigned int)out ))
		return 0;

	__asm__ __volatile__(
		"lqc2		$vf1, 0x00(%1)\n"
		"lqc2		$vf2, 0x10(%1)\n"
		"lqc2		$vf3, 0x20(%1)\n"
		"lqc2		$vf4, 0x00(%0)\n"
		"lqc2		$vf5, 0x10(%0)\n"
		"lqc2		$vf6, 0x20(%0)\n"
		// row = in1[r][0] * in2 row 0 + in1[r][1] * row 1 + in1[r][2] * row 2,
		// and vf0 = (0 0 0 1) adds in1[r][3] to w
		"vmulax.xyzw	$ACC, $vf1, $vf4x\n"
		"vmadday.xyzw	$ACC, $vf2, $vf4y\n"
		"vmaddaz.xyzw	$ACC, $vf3, $vf4z\n"
		"vmaddw.xyzw	$vf7, $vf0, $vf4w\n"
		"vmulax.xyzw	$ACC, $vf1, $vf5x\n"
		"vmadday.xyzw	$ACC, $vf2, $vf5y\n"
		"vmaddaz.xyzw	$ACC, $vf3, $vf5z\n"
		"vmaddw.xyzw	$vf8, $vf0, $vf5w\n"
		"vmulax.xyzw	$ACC, $vf1, $vf6x\n"
		"vmadday.xyzw	$ACC, $vf2, $vf6y\n"
		"vmaddaz.xyzw	$ACC, $vf3, $vf6z\n"
		"vmaddw.xyzw	$vf9, $vf0, $vf6w\n"
		"sqc2		$vf7, 0x00(%2)\n"
		"sqc2		$vf8, 0x10(%2)\n"
		"sqc2		$vf9, 0x20(%2)\n"
		:
		: "r"( in1 ), "r"( in2 ), "r"( out )
		: "memory"
	);
	return 1;
}

/* qt = normalize( p + t * ( q - p )): the slerp of two close quaternions */
static inline int PS2_VU0_QuaternionNlerp( const float p[4], const float q[4], float t, float qt[4] )
{
	float tv[4] PS2_ALIGN16 = { t, t, t, t };

	if( !PS2_VU0_ALIGNED( (unsigned int)p | (unsigned int)q | (unsigned int)qt ))
		return 0;

	__asm__ __volatile__(
		"lqc2		$vf1, 0(%0)\n"
		"lqc2		$vf2, 0(%1)\n"
		"lqc2		$vf3, 0(%2)\n"
		"vsub.xyzw	$vf4, $vf2, $vf1\n"
		"vmulx.xyzw	$vf4, $vf4, $vf3x\n"
		"vadd.xyzw	$vf5, $vf1, $vf4\n"
		"vmul.xyzw	$vf6, $vf5, $vf5\n"
		"vaddy.x	$vf7, $vf6, $vf6y\n"
		"vaddz.x	$vf7, $vf7, $vf6z\n"
		"vaddw.x	$vf7, $vf7, $vf6w\n"
		"vrsqrt		$Q, $vf0w, $vf7x\n"
		"vwaitq\n"
		"vmulq.xyzw	$vf5, $vf5, $Q\n"
		"sqc2		$vf5, 0(%3)\n"
		:
		: "r"( p ), "r"( q ), "r"( tv ), "r"( qt )
		: "memory"
	);
	return 1;
}

#else

#define PS2_ALIGN16

#endif // __PS2__ && !PS2_NO_VU0

#endif // PS2_VU0_H
