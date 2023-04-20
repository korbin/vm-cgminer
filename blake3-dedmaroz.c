#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define LEN 64  			

/*-----------------------------------------------------------------------------*
* BLAKE3 END  	    													       *
*  				                        									   *
* Input: 64B data, 32B midstate												   *
* Output: 256bit hash   					                                   *
*-----------------------------------------------------------------------------*/
void BLAKE3_END(uint8_t *p_in, uint8_t *p_inm, uint8_t *p_out)
{
	#define GET_ULONG(n,b,i)                   \
	{                                          \
		(n) = ( (uint32_t) (b)[(i) + 3] << 24 )  \
			| ( (uint32_t) (b)[(i) + 2] << 16 )  \
			| ( (uint32_t) (b)[(i) + 1] <<  8 )  \
			| ( (uint32_t) (b)[(i)    ]       ); \
	}

	#define PUT_ULONG(n,b,i)               \
	{                                      \
		(b)[(i) + 3] = (uint8_t)((n) >> 24); \
		(b)[(i) + 2] = (uint8_t)((n) >> 16); \
		(b)[(i) + 1] = (uint8_t)((n) >>  8); \
		(b)[(i)    ] = (uint8_t)((n)      ); \
	}

	// *** startovaci konstanty pro blake256 jsou stejne jako u sha256 ***
	uint32_t startConst[8] =
	{
		0x6a09e667,
		0xbb67ae85,
		0x3c6ef372,
		0xa54ff53a,
		0x510e527f,
		0x9b05688c,
		0x1f83d9ab,
		0x5be0cd19
	};

	uint32_t sigma[7][16] =
	{
		{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
	    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
	    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
	    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
	    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
	    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
	    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}			
	};

	// *** BLAKE3 operations ***
	#define SHR(x,n) ((x & 0xFFFFFFFF) >> n)
	#define ROT32(x,n) (SHR(x,n) | (x << (32 - n)))
	#define ADD32(x,y) ((uint32_t)((x) + (y)))
	#define XOR32(x,y) ((uint32_t)((x) ^ (y)))

	#define G32(a,b,c,d,i) do{   												   \
		v[a] = m[sigma[round][i]] + ADD32(v[a],v[b]);                              \
		v[d] = ROT32(XOR32(v[d],v[a]),16);										   \
		v[c] = ADD32(v[c],v[d]);												   \
		v[b] = ROT32(XOR32(v[b],v[c]),12);										   \
		v[a] = m[sigma[round][i+1]]+ADD32(v[a],v[b]); 							   \
		v[d] = ROT32(XOR32(v[d],v[a]), 8);										   \
		v[c] = ADD32(v[c],v[d]);												   \
		v[b] = ROT32(XOR32(v[b],v[c]), 7);										   \
	}while(0)

	uint8_t i;
	uint8_t dataBlock[64];
	uint8_t final[32];
	uint8_t mid[32];
	uint8_t midstate[32];
	uint32_t round;
	uint32_t v_pom[8];
	uint32_t pomHash[8];
	uint32_t v[16];
	uint32_t m[16];

	// *******************
	// *** FIRST ROUND ***
	// *******************
	// *** ULOZENI MIDSTATU ***
	for(i = 0; i < 32; i++)
	{
		midstate[i] = *p_inm++;
	}
	
	GET_ULONG(v[ 0], midstate,  0) 
	GET_ULONG(v[ 1], midstate,  4)
	GET_ULONG(v[ 2], midstate,  8)
	GET_ULONG(v[ 3], midstate, 12)
	GET_ULONG(v[ 4], midstate, 16)
	GET_ULONG(v[ 5], midstate, 20)
	GET_ULONG(v[ 6], midstate, 24)
	GET_ULONG(v[ 7], midstate, 28)
	
	// *** INICIALIZACE ***
	//v[ 0] = startConst[0]; // 0x6a09e667
	//v[ 1] = startConst[1]; // 0xbb67ae85
	//v[ 2] = startConst[2]; // 0x3c6ef372
	//v[ 3] = startConst[3]; // 0xa54ff53a
	//v[ 4] = startConst[4]; // 0x510e527f
	//v[ 5] = startConst[5]; // 0x9b05688c
	//v[ 6] = startConst[6]; // 0x1f83d9ab
	//v[ 7] = startConst[7]; // 0x5be0cd19

	v[ 8] = startConst[0]; // 0x6a09e667
	v[ 9] = startConst[1]; // 0xbb67ae85
	v[10] = startConst[2]; // 0x3c6ef372
	v[11] = startConst[3]; // 0xa54ff53a
	v[12] = 0; 
	v[13] = 0; 
	v[14] = 52; // data len 
	v[15] = 10; // root, end

	v_pom[0] = v[0];
	v_pom[1] = v[1];
	v_pom[2] = v[2];
	v_pom[3] = v[3];
	v_pom[4] = v[4];
	v_pom[5] = v[5];
	v_pom[6] = v[6];
	v_pom[7] = v[7];

	// *** ULOZENI VSTUPNICH DAT ***
	for(i = 0; i < 64; i++)
	{
		dataBlock[i] = *p_in++;
	}

	GET_ULONG(m[ 0], dataBlock,  0) 
	GET_ULONG(m[ 1], dataBlock,  4)
	GET_ULONG(m[ 2], dataBlock,  8)
	GET_ULONG(m[ 3], dataBlock, 12)
	GET_ULONG(m[ 4], dataBlock, 16)
	GET_ULONG(m[ 5], dataBlock, 20)
	GET_ULONG(m[ 6], dataBlock, 24)
	GET_ULONG(m[ 7], dataBlock, 28)
	GET_ULONG(m[ 8], dataBlock, 32)
	GET_ULONG(m[ 9], dataBlock, 36)
	GET_ULONG(m[10], dataBlock, 40)
	GET_ULONG(m[11], dataBlock, 44)
	GET_ULONG(m[12], dataBlock, 48)
	GET_ULONG(m[13], dataBlock, 52)
	GET_ULONG(m[14], dataBlock, 56)
	GET_ULONG(m[15], dataBlock, 60)

	// *** HLAVNI VYPOCET ***	
	for(round = 0; round < 7; ++round) //  BLAKE3 - 7rounds
	{
		G32(0, 4, 8,12, 0);
		G32(1, 5, 9,13, 2);
		G32(2, 6,10,14, 4);
		G32(3, 7,11,15, 6);
		G32(3, 4, 9,14,14);
		G32(2, 7, 8,13,12);
		G32(0, 5,10,15, 8);
		G32(1, 6,11,12,10);
	}

	// *** FINALNI UPRAVA DAT ***
	pomHash[0] = v[0] ^ v[ 8]; 
	pomHash[1] = v[1] ^ v[ 9];
	pomHash[2] = v[2] ^ v[10];
	pomHash[3] = v[3] ^ v[11];
	pomHash[4] = v[4] ^ v[12];
	pomHash[5] = v[5] ^ v[13];
	pomHash[6] = v[6] ^ v[14];
	pomHash[7] = v[7] ^ v[15];
	
	// *** ULOZENI FINALNICH 256 BITU DO VYSTUPNIHO BUFFERU ***
	PUT_ULONG(pomHash[0], final,  0)
	PUT_ULONG(pomHash[1], final,  4)
	PUT_ULONG(pomHash[2], final,  8)
	PUT_ULONG(pomHash[3], final, 12)
	PUT_ULONG(pomHash[4], final, 16)
	PUT_ULONG(pomHash[5], final, 20)
	PUT_ULONG(pomHash[6], final, 24)
	PUT_ULONG(pomHash[7], final, 28) // pozor, zase otoceny indiani !!!

	for(i = 0; i < 32; i++)
	{
		*p_out++ = final[i];
	}
}

/*-----------------------------------------------------------------------------*
* BLAKE3 MAIN  	    													       *
*  				                        									   *
* Input: 64B data, 32B midstate												   *
* Output: 256bit midstate					                                   *
*-----------------------------------------------------------------------------*/
void BLAKE3_MAIN(uint8_t *p_in, uint8_t *p_inm, uint8_t *p_out)
{
	#define GET_ULONG(n,b,i)                   \
	{                                          \
		(n) = ( (uint32_t) (b)[(i) + 3] << 24 )  \
			| ( (uint32_t) (b)[(i) + 2] << 16 )  \
			| ( (uint32_t) (b)[(i) + 1] <<  8 )  \
			| ( (uint32_t) (b)[(i)    ]       ); \
	}

	#define PUT_ULONG(n,b,i)               \
	{                                      \
		(b)[(i) + 3] = (uint8_t)((n) >> 24); \
		(b)[(i) + 2] = (uint8_t)((n) >> 16); \
		(b)[(i) + 1] = (uint8_t)((n) >>  8); \
		(b)[(i)    ] = (uint8_t)((n)      ); \
	}

	// *** startovaci konstanty pro blake256 jsou stejne jako u sha256 ***
	uint32_t startConst[8] =
	{
		0x6a09e667,
		0xbb67ae85,
		0x3c6ef372,
		0xa54ff53a,
		0x510e527f,
		0x9b05688c,
		0x1f83d9ab,
		0x5be0cd19
	};

	uint32_t sigma[7][16] =
	{
		{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
	    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
	    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
	    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
	    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
	    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
	    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}			
	};

	// *** BLAKE3 operations ***
	#define SHR(x,n) ((x & 0xFFFFFFFF) >> n)
	#define ROT32(x,n) (SHR(x,n) | (x << (32 - n)))
	#define ADD32(x,y) ((uint32_t)((x) + (y)))
	#define XOR32(x,y) ((uint32_t)((x) ^ (y)))

	#define G32(a,b,c,d,i) do{   												   \
		v[a] = m[sigma[round][i]] + ADD32(v[a],v[b]);                              \
		v[d] = ROT32(XOR32(v[d],v[a]),16);										   \
		v[c] = ADD32(v[c],v[d]);												   \
		v[b] = ROT32(XOR32(v[b],v[c]),12);										   \
		v[a] = m[sigma[round][i+1]]+ADD32(v[a],v[b]); 							   \
		v[d] = ROT32(XOR32(v[d],v[a]), 8);										   \
		v[c] = ADD32(v[c],v[d]);												   \
		v[b] = ROT32(XOR32(v[b],v[c]), 7);										   \
	}while(0)

	uint8_t i;
	uint8_t dataBlock[64];
	uint8_t final[32];
	uint8_t mid[32];
	uint8_t midstate[32];
	uint32_t round;
	uint32_t v_pom[8];
	uint32_t pomHash[8];
	uint32_t v[16];
	uint32_t m[16];

	// *******************
	// *** FIRST ROUND ***
	// *******************
	// *** ULOZENI MIDSTATU ***
	for(i = 0; i < 32; i++)
	{
		midstate[i] = *p_inm++;
	}
	
	GET_ULONG(v[ 0], midstate,  0) 
	GET_ULONG(v[ 1], midstate,  4)
	GET_ULONG(v[ 2], midstate,  8)
	GET_ULONG(v[ 3], midstate, 12)
	GET_ULONG(v[ 4], midstate, 16)
	GET_ULONG(v[ 5], midstate, 20)
	GET_ULONG(v[ 6], midstate, 24)
	GET_ULONG(v[ 7], midstate, 28)
	
	// *** INICIALIZACE ***
	//v[ 0] = startConst[0]; // 0x6a09e667
	//v[ 1] = startConst[1]; // 0xbb67ae85
	//v[ 2] = startConst[2]; // 0x3c6ef372
	//v[ 3] = startConst[3]; // 0xa54ff53a
	//v[ 4] = startConst[4]; // 0x510e527f
	//v[ 5] = startConst[5]; // 0x9b05688c
	//v[ 6] = startConst[6]; // 0x1f83d9ab
	//v[ 7] = startConst[7]; // 0x5be0cd19

	v[ 8] = startConst[0]; // 0x6a09e667
	v[ 9] = startConst[1]; // 0xbb67ae85
	v[10] = startConst[2]; // 0x3c6ef372
	v[11] = startConst[3]; // 0xa54ff53a
	v[12] = 0; 
	v[13] = 0; 
	v[14] = 64; // data len 
	v[15] = 0; // 

	v_pom[0] = v[0];
	v_pom[1] = v[1];
	v_pom[2] = v[2];
	v_pom[3] = v[3];
	v_pom[4] = v[4];
	v_pom[5] = v[5];
	v_pom[6] = v[6];
	v_pom[7] = v[7];

	// *** ULOZENI VSTUPNICH DAT ***
	for(i = 0; i < 64; i++)
	{
		dataBlock[i] = *p_in++;
	}

	GET_ULONG(m[ 0], dataBlock,  0) 
	GET_ULONG(m[ 1], dataBlock,  4)
	GET_ULONG(m[ 2], dataBlock,  8)
	GET_ULONG(m[ 3], dataBlock, 12)
	GET_ULONG(m[ 4], dataBlock, 16)
	GET_ULONG(m[ 5], dataBlock, 20)
	GET_ULONG(m[ 6], dataBlock, 24)
	GET_ULONG(m[ 7], dataBlock, 28)
	GET_ULONG(m[ 8], dataBlock, 32)
	GET_ULONG(m[ 9], dataBlock, 36)
	GET_ULONG(m[10], dataBlock, 40)
	GET_ULONG(m[11], dataBlock, 44)
	GET_ULONG(m[12], dataBlock, 48)
	GET_ULONG(m[13], dataBlock, 52)
	GET_ULONG(m[14], dataBlock, 56)
	GET_ULONG(m[15], dataBlock, 60)

	// *** HLAVNI VYPOCET ***	
	for(round = 0; round < 7; ++round) //  BLAKE3 - 7rounds
	{
		G32(0, 4, 8,12, 0);
		G32(1, 5, 9,13, 2);
		G32(2, 6,10,14, 4);
		G32(3, 7,11,15, 6);
		G32(3, 4, 9,14,14);
		G32(2, 7, 8,13,12);
		G32(0, 5,10,15, 8);
		G32(1, 6,11,12,10);
	}

	// *** FINALNI UPRAVA DAT ***
	pomHash[0] = v[0] ^ v[ 8]; 
	pomHash[1] = v[1] ^ v[ 9];
	pomHash[2] = v[2] ^ v[10];
	pomHash[3] = v[3] ^ v[11];
	pomHash[4] = v[4] ^ v[12];
	pomHash[5] = v[5] ^ v[13];
	pomHash[6] = v[6] ^ v[14];
	pomHash[7] = v[7] ^ v[15];
	
	// *** ULOZENI FINALNICH 256 BITU DO VYSTUPNIHO BUFFERU ***
	PUT_ULONG(pomHash[0], final,  0)
	PUT_ULONG(pomHash[1], final,  4)
	PUT_ULONG(pomHash[2], final,  8)
	PUT_ULONG(pomHash[3], final, 12)
	PUT_ULONG(pomHash[4], final, 16)
	PUT_ULONG(pomHash[5], final, 20)
	PUT_ULONG(pomHash[6], final, 24)
	PUT_ULONG(pomHash[7], final, 28) // pozor, zase otoceny indiani !!!

	for(i = 0; i < 32; i++)
	{
		*p_out++ = final[i];
	}
}

/*-----------------------------------------------------------------------------*
* BLAKE3 START	    													       *
*  				                        									   *
* Input: 64B 																   *
* Output: 256bit midstate					                                   *
*-----------------------------------------------------------------------------*/
void BLAKE3_START(uint8_t *p_in, uint8_t *p_out)
{
	#define GET_ULONG(n,b,i)                   \
	{                                          \
		(n) = ( (uint32_t) (b)[(i) + 3] << 24 )  \
			| ( (uint32_t) (b)[(i) + 2] << 16 )  \
			| ( (uint32_t) (b)[(i) + 1] <<  8 )  \
			| ( (uint32_t) (b)[(i)    ]       ); \
	}

	#define PUT_ULONG(n,b,i)               \
	{                                      \
		(b)[(i) + 3] = (uint8_t)((n) >> 24); \
		(b)[(i) + 2] = (uint8_t)((n) >> 16); \
		(b)[(i) + 1] = (uint8_t)((n) >>  8); \
		(b)[(i)    ] = (uint8_t)((n)      ); \
	}

	// *** startovaci konstanty pro blake256 jsou stejne jako u sha256 ***
	uint32_t startConst[8] =
	{
		0x6a09e667,
		0xbb67ae85,
		0x3c6ef372,
		0xa54ff53a,
		0x510e527f,
		0x9b05688c,
		0x1f83d9ab,
		0x5be0cd19
	};

	uint32_t sigma[7][16] =
	{
		{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
	    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
	    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
	    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
	    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
	    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
	    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}			
	};

	// *** BLAKE3 operations ***
	#define SHR(x,n) ((x & 0xFFFFFFFF) >> n)
	#define ROT32(x,n) (SHR(x,n) | (x << (32 - n)))
	#define ADD32(x,y) ((uint32_t)((x) + (y)))
	#define XOR32(x,y) ((uint32_t)((x) ^ (y)))

	#define G32(a,b,c,d,i) do{   												   \
		v[a] = m[sigma[round][i]] + ADD32(v[a],v[b]);                              \
		v[d] = ROT32(XOR32(v[d],v[a]),16);										   \
		v[c] = ADD32(v[c],v[d]);												   \
		v[b] = ROT32(XOR32(v[b],v[c]),12);										   \
		v[a] = m[sigma[round][i+1]]+ADD32(v[a],v[b]); 							   \
		v[d] = ROT32(XOR32(v[d],v[a]), 8);										   \
		v[c] = ADD32(v[c],v[d]);												   \
		v[b] = ROT32(XOR32(v[b],v[c]), 7);										   \
	}while(0)

	uint8_t i;
	uint8_t dataBlock[64];
	uint8_t final[32];
	uint8_t mid[32];
	uint32_t round;
	uint32_t v_pom[8];
	uint32_t pomHash[8];
	uint32_t v[16];
	uint32_t m[16];

	// *******************
	// *** FIRST ROUND ***
	// *******************

	// *** INICIALIZACE ***
	v[ 0] = startConst[0]; // 0x6a09e667
	v[ 1] = startConst[1]; // 0xbb67ae85
	v[ 2] = startConst[2]; // 0x3c6ef372
	v[ 3] = startConst[3]; // 0xa54ff53a
	v[ 4] = startConst[4]; // 0x510e527f
	v[ 5] = startConst[5]; // 0x9b05688c
	v[ 6] = startConst[6]; // 0x1f83d9ab
	v[ 7] = startConst[7]; // 0x5be0cd19

	v[ 8] = startConst[0]; // 0x6a09e667
	v[ 9] = startConst[1]; // 0xbb67ae85
	v[10] = startConst[2]; // 0x3c6ef372
	v[11] = startConst[3]; // 0xa54ff53a
	v[12] = 0; 
	v[13] = 0; 
	v[14] = 64; // data len 
	v[15] = 1; // start

	v_pom[0] = v[0];
	v_pom[1] = v[1];
	v_pom[2] = v[2];
	v_pom[3] = v[3];
	v_pom[4] = v[4];
	v_pom[5] = v[5];
	v_pom[6] = v[6];
	v_pom[7] = v[7];

	// *** ULOZENI VSTUPNICH DAT ***
	for(i = 0; i < 64; i++)
	{
		dataBlock[i] = *p_in++;
	}

	GET_ULONG(m[ 0], dataBlock,  0) 
	GET_ULONG(m[ 1], dataBlock,  4)
	GET_ULONG(m[ 2], dataBlock,  8)
	GET_ULONG(m[ 3], dataBlock, 12)
	GET_ULONG(m[ 4], dataBlock, 16)
	GET_ULONG(m[ 5], dataBlock, 20)
	GET_ULONG(m[ 6], dataBlock, 24)
	GET_ULONG(m[ 7], dataBlock, 28)
	GET_ULONG(m[ 8], dataBlock, 32)
	GET_ULONG(m[ 9], dataBlock, 36)
	GET_ULONG(m[10], dataBlock, 40)
	GET_ULONG(m[11], dataBlock, 44)
	GET_ULONG(m[12], dataBlock, 48)
	GET_ULONG(m[13], dataBlock, 52)
	GET_ULONG(m[14], dataBlock, 56)
	GET_ULONG(m[15], dataBlock, 60)

	// *** HLAVNI VYPOCET ***	
	for(round = 0; round < 7; ++round) //  BLAKE3 - 7rounds
	{
		G32(0, 4, 8,12, 0);
		G32(1, 5, 9,13, 2);
		G32(2, 6,10,14, 4);
		G32(3, 7,11,15, 6);
		G32(3, 4, 9,14,14);
		G32(2, 7, 8,13,12);
		G32(0, 5,10,15, 8);
		G32(1, 6,11,12,10);
	}

	// *** FINALNI UPRAVA DAT ***
	pomHash[0] = v[0] ^ v[ 8]; 
	pomHash[1] = v[1] ^ v[ 9];
	pomHash[2] = v[2] ^ v[10];
	pomHash[3] = v[3] ^ v[11];
	pomHash[4] = v[4] ^ v[12];
	pomHash[5] = v[5] ^ v[13];
	pomHash[6] = v[6] ^ v[14];
	pomHash[7] = v[7] ^ v[15];
	
	// *** ULOZENI FINALNICH 256 BITU DO VYSTUPNIHO BUFFERU ***
	PUT_ULONG(pomHash[0], final,  0)
	PUT_ULONG(pomHash[1], final,  4)
	PUT_ULONG(pomHash[2], final,  8)
	PUT_ULONG(pomHash[3], final, 12)
	PUT_ULONG(pomHash[4], final, 16)
	PUT_ULONG(pomHash[5], final, 20)
	PUT_ULONG(pomHash[6], final, 24)
	PUT_ULONG(pomHash[7], final, 28) // pozor, zase otoceny indiani !!!

	for(i = 0; i < 32; i++)
	{
		*p_out++ = final[i];
	}
}
	
int test(void)
{
	unsigned char i, j, char1, char2;    
	unsigned char in[LEN];
	unsigned char inhex[LEN/2];    	
	unsigned char hash[32];    		
 
    printf("Enter BLAKE3 32B input data: ");
    scanf("%s", &in);        
    
    FILE * p_soubor = fopen("output_hash.txt", "w");
    if (p_soubor == NULL)
    {
        printf("Soubor se nepodaøilo otevøít pro zápis, zkontrolujte prosím oprávnìní.");
        return 1;
    }
		
	// *** BIN2HEX ***
	for(i = 0; i < strlen(in); i++, j+=2)
	{	
		if (in[j] >= 0x30 && in[j] <= 0x39)
		{
			char1 = in[j] - 0x30;
		}
		else if (in[j] >= 0x41 && in[j] <= 0x46)
		{
			char1 = in[j] - 0x37;		
		}	
		else if (in[j] >= 0x61 && in[j] <= 0x66)
		{
			char1 = in[j] - 0x57;		
		}
		
		if (in[j+1] >= 0x30 && in[j+1] <= 0x39)
		{
			char2 = in[j+1] - 0x30;
		}
		else if (in[j+1] >= 0x41 && in[j+1] <= 0x46)
		{
			char2 = in[j+1] - 0x37;
		}
		else if (in[j+1] >= 0x61 && in[j+1] <= 0x66)
		{
			char2 = in[j+1] - 0x57;		
		}
		
		inhex[i] = char1;
		inhex[i] <<= 4;
		inhex[i] |= char2;
	}								
	
	// ################################
	// ##### BLAKE3 HASH FUNCTION #####
	// ################################
	// in 180B
	// 91c90001726b42f62c8400000000000000029ca3814215d060fcc2ef2c003b21e3b8ea63385c9cd17dddc21bd9a7ca942a0e137837e54a4d54310a09788b5326
	// 5ec3003416d97cb02987eb735eda3e492f73ed94208d264989b159a03b7151dd66fdba748fdcf6693dc84517000000000010ee5b8aefa76ca17b944ad3c6676c
	// 91deb73f0725dc75d06aeae20d2e292587010000637a4d565253356d4f513235466454496b41593877677a566e6f467044746754
	// hash
	// 00000000181aedf237b0ad2a7b9ebadd022af231e7e398c9586bf7f5520a58ad
	
	unsigned char data1[64] = 
	{
		0x91,0xc9,0x00,0x01,0x72,0x6b,0x42,0xf6,0x2c,0x84,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x02,0x9c,0xa3,0x81,0x42,0x15,0xd0,0x60,0xfc,0xc2,0xef,0x2c,0x00,0x3b,0x21,
		0xe3,0xb8,0xea,0x63,0x38,0x5c,0x9c,0xd1,0x7d,0xdd,0xc2,0x1b,0xd9,0xa7,0xca,0x94,
		0x2a,0x0e,0x13,0x78,0x37,0xe5,0x4a,0x4d,0x54,0x31,0x0a,0x09,0x78,0x8b,0x53,0x26
	};
	
	unsigned char data2[64] =
	{
		0x5e,0xc3,0x00,0x34,0x16,0xd9,0x7c,0xb0,0x29,0x87,0xeb,0x73,0x5e,0xda,0x3e,0x49,
		0x2f,0x73,0xed,0x94,0x20,0x8d,0x26,0x49,0x89,0xb1,0x59,0xa0,0x3b,0x71,0x51,0xdd,
		0x66,0xfd,0xba,0x74,0x8f,0xdc,0xf6,0x69,0x3d,0xc8,0x45,0x17,0x00,0x00,0x00,0x00,
		0x00,0x10,0xee,0x5b,0x8a,0xef,0xa7,0x6c,0xa1,0x7b,0x94,0x4a,0xd3,0xc6,0x67,0x6c
	}; 
	
	unsigned char data3[64] =
	{
		0x91,0xde,0xb7,0x3f,0x07,0x25,0xdc,0x75,0xd0,0x6a,0xea,0xe2,0x0d,0x2e,0x29,0x25,
		0x87,0x01,0x00,0x00,0x63,0x7a,0x4d,0x56,0x52,0x53,0x35,0x6d,0x4f,0x51,0x32,0x35,
		0x46,0x64,0x54,0x49,0x6b,0x41,0x59,0x38,0x77,0x67,0x7a,0x56,0x6e,0x6f,0x46,0x70,
		0x44,0x74,0x67,0x54,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	};
	
	unsigned char midstate1[32];
	unsigned char midstate2[32];	
	
	BLAKE3_START(&data1[0], &midstate1[0]);	
	
	printf("\nmidstate 1:%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", 
		midstate1[ 0], midstate1[ 1], midstate1[ 2], midstate1[ 3], midstate1[ 4], midstate1[ 5], midstate1[ 6], midstate1[ 7], 
		midstate1[ 8], midstate1[ 9], midstate1[10], midstate1[11], midstate1[12], midstate1[13], midstate1[14], midstate1[15],
		midstate1[16], midstate1[17], midstate1[18], midstate1[19], midstate1[20], midstate1[21], midstate1[22], midstate1[23], 
		midstate1[24], midstate1[25], midstate1[26], midstate1[27], midstate1[28], midstate1[29], midstate1[30], midstate1[31]
	);
	
	BLAKE3_MAIN(&data2[0], &midstate1[0], &midstate2[0]);	
	
	printf("midstate 2:%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", 
		midstate2[ 0], midstate2[ 1], midstate2[ 2], midstate2[ 3], midstate2[ 4], midstate2[ 5], midstate2[ 6], midstate2[ 7], 
		midstate2[ 8], midstate2[ 9], midstate2[10], midstate2[11], midstate2[12], midstate2[13], midstate2[14], midstate2[15],
		midstate2[16], midstate2[17], midstate2[18], midstate2[19], midstate2[20], midstate2[21], midstate2[22], midstate2[23], 
		midstate2[24], midstate2[25], midstate2[26], midstate2[27], midstate2[28], midstate2[29], midstate2[30], midstate2[31]
	);
	
	fprintf(p_soubor, "midstate:%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", 
		midstate2[ 0], midstate2[ 1], midstate2[ 2], midstate2[ 3], midstate2[ 4], midstate2[ 5], midstate2[ 6], midstate2[ 7], 
		midstate2[ 8], midstate2[ 9], midstate2[10], midstate2[11], midstate2[12], midstate2[13], midstate2[14], midstate2[15],
		midstate2[16], midstate2[17], midstate2[18], midstate2[19], midstate2[20], midstate2[21], midstate2[22], midstate2[23], 
		midstate2[24], midstate2[25], midstate2[26], midstate2[27], midstate2[28], midstate2[29], midstate2[30], midstate2[31]
	);
	
	BLAKE3_END(&data3[0], &midstate2[0], &hash[0]);	
	
	printf("\nHash:%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", 
		hash[ 0], hash[ 1], hash[ 2], hash[ 3], hash[ 4], hash[ 5], hash[ 6], hash[ 7], 
		hash[ 8], hash[ 9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15],
		hash[16], hash[17], hash[18], hash[19], hash[20], hash[21], hash[22], hash[23], 
		hash[24], hash[25], hash[26], hash[27], hash[28], hash[29], hash[30], hash[31]
	);
		
    fprintf(p_soubor, "Hash:\n%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", 
		hash[ 0], hash[ 1], hash[ 2], hash[ 3], hash[ 4], hash[ 5], hash[ 6], hash[ 7], 
		hash[ 8], hash[ 9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15],
		hash[16], hash[17], hash[18], hash[19], hash[20], hash[21], hash[22], hash[23], 
		hash[24], hash[25], hash[26], hash[27], hash[28], hash[29], hash[30], hash[31]
	);
	
    if (fclose(p_soubor) == EOF)
    {
        printf("Soubor se nepodaøilo uzavøít.");
        return 1;
    }
}
