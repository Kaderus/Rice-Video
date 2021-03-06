/*
Copyright (C) 2003-2009 Rice1964

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "..\stdafx.h"
#include "BMGDLL.h"
#include "../Utility/util.h"

CRender * CRender::g_pRender=NULL;
int CRender::gRenderReferenceCount=0;

Matrix4x4 reverseXY(-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
Matrix4x4 reverseY(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);

//========================================================================
CRender * CRender::GetRender(void)
{
	if( CRender::g_pRender == NULL )
	{
		ErrorMsg("Failed to get render, g_pRender is NULL");
		exit(0);
	}
	else
		return CRender::g_pRender;
}

bool CRender::IsAvailable()
{
	return CRender::g_pRender != NULL;
}

CRender::CRender() :
	m_bZUpdate(FALSE),
	m_bZCompare(FALSE),
	m_dwZBias(0),

	m_dwAlpha(0xFF),
	
	m_dwMinFilter(D3DTEXF_POINT),
	m_dwMagFilter(D3DTEXF_POINT)
{
	int i;
	InitRenderBase();

	for( i=0; i<MAX_TEXTURES; i++ )
	{
		g_textures[i].m_lpsTexturePtr = NULL;
		g_textures[i].m_pCTexture = NULL;
		
		g_textures[i].m_dwTileWidth = 64;		// Value doesn't really matter, as tex not set
		g_textures[i].m_dwTileHeight = 64;
		g_textures[i].m_fTexWidth = 64.0f;		// Value doesn't really matter, as tex not set
		g_textures[i].m_fTexHeight = 64.0f;

		TileUFlags[i] = TileVFlags[i] = D3DTADDRESS_CLAMP;
	}

	//Create the pixel shader, where going to assume the user has support for this
	m_pColorCombiner = new CDirectXPixelShaderCombiner(this);
	//Inititalize the pixel shader combiner
	m_pColorCombiner->Initialize();

}

CRender::~CRender()
{
	if( m_pColorCombiner != NULL )
	{
		SAFE_DELETE(m_pColorCombiner);
		m_pColorCombiner = NULL;
	}	
}

void CRender::ResetMatrices(uint32 size)
{
	Matrix4x4 mat;

	//Tigger's Honey Hunt
	if (size == 0)
		size = RICE_MATRIX_STACK;

	gRSP.mMatStackSize = (size > RICE_MATRIX_STACK) ? RICE_MATRIX_STACK : size;
	gRSP.mModelViewTop = 0;
	gRSP.mProjectionMat = gRSP.mModelViewStack[0] = gMatrixIdentity;
	gRSP.mWorldProjectValid = false;
}

void CRender::SetProjection(const u32 address, bool bReplace)
{
	if (bReplace)
	{
		MatrixFromN64FixedPoint(gRSP.mProjectionMat, address);

        // Hack needed to show flashing last heart and map arrows in Zelda OoT & MM
        // It renders at Z cordinate = 0.0f that gets clipped away
        // So we translate them a bit along Z to make them stick
        if (options.enableHackForGames == HACK_FOR_ZELDA || options.enableHackForGames == HACK_FOR_ZELDA_MM)
            gRSP.mProjectionMat.m43 += 0.5f;

	}
	else
	{
		MatrixFromN64FixedPoint(gRSP.mTempMat, address);
		MatrixMultiplyAligned(&gRSP.mProjectionMat, &gRSP.mTempMat, &gRSP.mProjectionMat);
	}

	gRSP.mWorldProjectValid = false;
}

void CRender::SetWorldView(const u32 address, bool bPush, bool bReplace)
{
	if (bPush && (gRSP.mModelViewTop < gRSP.mMatStackSize))
	{
		gRSP.mModelViewTop++;

		// We should store the current projection matrix...
		if (bReplace)
		{
			//Load Modelview matrix
			MatrixFromN64FixedPoint(gRSP.mModelViewStack[gRSP.mModelViewTop], address);
		}
		else // Multiply projection matrix
		{
			MatrixFromN64FixedPoint(gRSP.mTempMat, address);
			MatrixMultiplyAligned(&gRSP.mModelViewStack[gRSP.mModelViewTop], &gRSP.mTempMat, &gRSP.mModelViewStack[gRSP.mModelViewTop - 1]);
		}
	}
	else	// NoPush
	{
		if (bReplace)
		{
			// Load projection matrix
			MatrixFromN64FixedPoint(gRSP.mModelViewStack[gRSP.mModelViewTop], address);
		}
		else
		{
			// Multiply projection matrix
			MatrixFromN64FixedPoint(gRSP.mTempMat, address);
			MatrixMultiplyAligned(&gRSP.mModelViewStack[gRSP.mModelViewTop], &gRSP.mTempMat, &gRSP.mModelViewStack[gRSP.mModelViewTop]);
		}
	}

	gRSP.mWorldProjectValid = false;
}


void CRender::PopWorldView(u32 num)
{
	if (gRSP.mModelViewTop > (num - 1))
	{
		gRSP.mModelViewTop -= num;

		gRSP.mWorldProjectValid = false;
	}
}

void CRender::SetMux(uint32 dwMux0, uint32 dwMux1)
{
	uint64 tempmux = (((uint64)dwMux0) << 32) | (uint64)dwMux1;
	if( m_Mux != tempmux )
	{
		m_Mux = tempmux;
		m_pColorCombiner->UpdateCombiner(dwMux0, dwMux1);
	}
}


void CRender::SetCombinerAndBlender()
{
	InitOtherModes();
	
	CBlender::InitBlenderMode();

	m_pColorCombiner->InitCombinerMode();

	ApplyTextureFilter();
}

void CRender::RenderReset()
{
	UpdateClipRectangle();
	SetZBias(0);
	gRSP.numVertices = 0;
	gRSP.maxVertexID = 0;
	gRSP.curTile = 0;
	gRSP.fTexScaleX = 1/32.0f;
	gRSP.fTexScaleY = 1/32.0f;
}

bool CRender::FillRect(LONG nX0, LONG nY0, LONG nX1, LONG nY1, uint32 dwColor)
{
	LOG_UCODE("FillRect: X0=%d, Y0=%d, X1=%d, Y1=%d, Color=0x%8X", nX0, nY0, nX1, nY1, dwColor);

	if( g_CI.dwSize != TXT_SIZE_16b && frameBufferOptions.bIgnore )
		return true;

	if( status.bHandleN64RenderTexture)	
		status.bFrameBufferIsDrawn = true;

	SetFillMode(RICE_FILLMODE_SOLID);

	bool res=true;

	ZBufferEnable( FALSE );

	m_fillRectVtx[0].x = ViewPortTranslatei_x(nX0);
	m_fillRectVtx[0].y = ViewPortTranslatei_y(nY0);
	m_fillRectVtx[1].x = ViewPortTranslatei_x(nX1);
	m_fillRectVtx[1].y = ViewPortTranslatei_y(nY1);

	SetCombinerAndBlender();

	if( gRDP.otherMode.cycle_type  >= CYCLE_TYPE_COPY )
		ZBufferEnable(FALSE);

	float depth = (gRDP.otherMode.depth_source == 1 ? gRDP.fPrimitiveDepth : 0 );

	ApplyRDPScissor();
	TurnFogOnOff(false);
	res = RenderFillRect(dwColor, depth);
	TurnFogOnOff(gRDP.tnl.Fog);

	if( gRDP.otherMode.cycle_type  >= CYCLE_TYPE_COPY )
		ZBufferEnable(gRDP.tnl.Zbuffer);

	if( options.bWinFrameMode )	SetFillMode(RICE_FILLMODE_WINFRAME );

	DEBUGGER_PAUSE_AND_DUMP_COUNT_N( NEXT_FILLRECT, {DebuggerAppendMsg("FillRect: X0=%d, Y0=%d, X1=%d, Y1=%d, Color=0x%08X", nX0, nY0, nX1, nY1, dwColor);
			DebuggerAppendMsg("Pause after FillRect: Color=%08X\n", dwColor);if( logCombiners ) m_pColorCombiner->DisplayMuxString();});

	return res;
}


bool CRender::Line3D(uint32 dwV0, uint32 dwV1, uint32 dwWidth)
{
	LOG_UCODE("Line3D: Vtx0=%d, Vtx1=%d, Width=%d", dwV0, dwV1, dwWidth);
	if( !status.bCIBufferIsRendered ) g_pFrameBufferManager->ActiveTextureBuffer();

	m_line3DVtx[0].z = (g_vecProjected[dwV0].ProjectedPos.z + 1.0f) * 0.5f;
	m_line3DVtx[1].z = (g_vecProjected[dwV1].ProjectedPos.z + 1.0f) * 0.5f;

	if( m_line3DVtx[0].z != m_line3DVtx[1].z )  
		return false;

	if( status.bHandleN64RenderTexture ) 
	{
		g_pRenderTextureInfo->maxUsedHeight = g_pRenderTextureInfo->N64Height;
		status.bFrameBufferIsDrawn = true;
		status.bFrameBufferDrawnByTriangles = true;
	}

	m_line3DVtx[0].x = ViewPortTranslatef_x(g_vecProjected[dwV0].ProjectedPos.x);
	m_line3DVtx[0].y = ViewPortTranslatef_y(g_vecProjected[dwV0].ProjectedPos.y);
	m_line3DVtx[0].rhw = g_vecProjected[dwV0].ProjectedPos.w;
	m_line3DVtx[0].dcDiffuse = g_dwVtxDifColor[dwV0];

	m_line3DVtx[1].x = ViewPortTranslatef_x(g_vecProjected[dwV1].ProjectedPos.x);
	m_line3DVtx[1].y = ViewPortTranslatef_y(g_vecProjected[dwV1].ProjectedPos.y);
	m_line3DVtx[1].rhw = g_vecProjected[dwV1].ProjectedPos.w;
	m_line3DVtx[1].dcDiffuse = g_dwVtxDifColor[dwV1];

	float width = dwWidth*0.5f+1.5f;

	if( m_line3DVtx[0].y == m_line3DVtx[1].y )
	{
		m_line3DVector[0].x = m_line3DVector[1].x = m_line3DVtx[0].x;
		m_line3DVector[2].x = m_line3DVector[3].x = m_line3DVtx[1].x;

		m_line3DVector[0].y = m_line3DVector[2].y = m_line3DVtx[0].y-width/2*windowSetting.fMultY;
		m_line3DVector[1].y = m_line3DVector[3].y = m_line3DVtx[0].y+width/2*windowSetting.fMultY;
	}
	else
	{
		m_line3DVector[0].y = m_line3DVector[1].y = m_line3DVtx[0].y;
		m_line3DVector[2].y = m_line3DVector[3].y = m_line3DVtx[1].y;

		m_line3DVector[0].x = m_line3DVector[2].x = m_line3DVtx[0].x-width/2*windowSetting.fMultX;
		m_line3DVector[1].x = m_line3DVector[3].x = m_line3DVtx[0].x+width/2*windowSetting.fMultX;
	}

	SetCombinerAndBlender();

	bool res=RenderLine3D();

	DEBUGGER_PAUSE_AND_DUMP_COUNT_N(NEXT_FLUSH_TRI, {
		DebuggerAppendMsg("Pause after Line3D: v%d(%f,%f,%f), v%d(%f,%f,%f), Width=%d\n", dwV0, m_line3DVtx[0].x, m_line3DVtx[0].y, m_line3DVtx[0].z, 
			dwV1, m_line3DVtx[1].x, m_line3DVtx[1].y, m_line3DVtx[1].z, dwWidth);
	});

	DEBUGGER_PAUSE_AND_DUMP_COUNT_N(NEXT_OBJ_TXT_CMD, {
		DebuggerAppendMsg("Pause after Line3D: v%d(%f,%f,%f), v%d(%f,%f,%f), Width=%d\n", dwV0, m_line3DVtx[0].x, m_line3DVtx[0].y, m_line3DVtx[0].z, 
			dwV1, m_line3DVtx[1].x, m_line3DVtx[1].y, m_line3DVtx[1].z, dwWidth);
	});

	return res;
}

bool CRender::RemapTextureCoordinate
	(float t0, float t1, uint32 tileWidth, uint32 mask, float textureWidth, float &u0, float &u1)
{
	int s0 = (int)t0;
	int s1 = (int)t1;
	int width = mask>0 ? (1<<mask) : tileWidth;
	if( width == 0 ) return false;

	int divs0 = s0/width; if( divs0*width > s0 )	divs0--;
	int divs1 = s1/width; if( divs1*width > s1 )	divs1--;

	if( divs0 == divs1 )
	{
		s0 -= divs0*width;
		s1 -= divs1*width;
		//if( s0 > s1 )	
		//	s0++;
		//else if( s1 > s0 )	
		//	s1++;
		u0 = s0/textureWidth;
		u1 = s1/textureWidth;

		return true;
	}
	else if( divs0+1 == divs1 && s0%width==0 && s1%width == 0 )
	{
		u0 = 0;
		u1 = tileWidth/textureWidth;
		return true;
	}
	else if( divs0 == divs1+1 && s0%width==0 && s1%width == 0 )
	{
		u1 = 0;
		u0 = tileWidth/textureWidth;
		return true;
	}
	else
	{
		//if( s0 > s1 )	
		//{
			//s0++;
		//	u0 = s0/textureWidth;
		//}
		//else if( s1 > s0 )	
		//{
			//s1++;
		//	u1 = s1/textureWidth;
		//}

		return false;
	}
}

bool CRender::TexRect(LONG nX0, LONG nY0, LONG nX1, LONG nY1, float fS0, float fT0, float fScaleS, float fScaleT, bool colorFlag, uint32 diffuseColor)
{
	if( options.enableHackForGames == HACK_FOR_DUKE_NUKEM )
	{
		colorFlag = true;
		diffuseColor = 0;
	}

	if( options.enableHackForGames == HACK_FOR_BANJO_TOOIE )
	{
		// Hack for Banjo shadow in Banjo Tooie
		if( g_TI.dwWidth == g_CI.dwWidth && g_TI.dwFormat == TXT_FMT_CI && g_TI.dwSize == TXT_SIZE_8b )
		{
			if( nX0 == fS0 && nY0 == fT0 )//&& nX0 > 90 && nY0 > 130 && nX1-nX0 > 80 && nY1-nY0 > 20 )
			{
				// Skip the text rect
				return true;
			}
		}
	}

	if( status.bN64IsDrawingTextureBuffer )
	{
		if( frameBufferOptions.bIgnore || ( frameBufferOptions.bIgnoreRenderTextureIfHeightUnknown && newRenderTextureInfo.knownHeight == 0 ) )
		{
			return true;
		}
	}

	PrepareTextures();

	if( status.bHandleN64RenderTexture && g_pRenderTextureInfo->CI_Info.dwSize == TXT_SIZE_8b )	
	{
		return true;
	}

	if( !IsTextureEnabled() &&  gRDP.otherMode.cycle_type  != CYCLE_TYPE_COPY )
	{
		FillRect(nX0, nY0, nX1, nY1, gRDP.primitiveColor);
		return true;
	}

	if( IsUsedAsDI(g_CI.dwAddr) && !status.bHandleN64RenderTexture )
	{
		status.bFrameBufferIsDrawn = true;
	}

	if( status.bHandleN64RenderTexture)
		status.bFrameBufferIsDrawn = true;

	LOG_UCODE("TexRect: X0=%d, Y0=%d, X1=%d, Y1=%d,\n\t\tfS0=%f, fT0=%f, ScaleS=%f, ScaleT=%f ",
		nX0, nY0, nX1, nY1, fS0, fT0, fScaleS, fScaleT);

	if( options.bEnableHacks )
	{
		// Goldeneye HACK
		if( options.bEnableHacks && nY1 - nY0 < 2 ) 
			nY1 = nY1+2;

		//// Text edge hack
		else if( gRDP.otherMode.cycle_type == CYCLE_TYPE_1 && fScaleS == 1 && fScaleT == 1 && 
			g_textures[gRSP.curTile].m_dwTileWidth == nX1-nX0+1 && g_textures[gRSP.curTile].m_dwTileHeight == nY1-nY0+1 &&
			g_textures[gRSP.curTile].m_dwTileWidth%2 == 0 && g_textures[gRSP.curTile].m_dwTileHeight%2 == 0 )
		{
			nY1++;
			nX1++;
		}
		else if( g_curRomInfo.bIncTexRectEdge )
		{
			nX1++;
			nY1++;
		}
	}


	// Scale to Actual texture coords
	// The two cases are to handle the oversized textures hack on voodoos

	SetCombinerAndBlender();
	
	if( gRDP.otherMode.cycle_type  >= CYCLE_TYPE_COPY || !gRDP.otherMode.z_cmp )
	{
		ZBufferEnable(FALSE);
	}

	CTexture *surf = g_textures[gRSP.curTile].m_pCTexture;
	RenderTexture &tex0 = g_textures[gRSP.curTile];
	Tile &tile0 = gRDP.tiles[gRSP.curTile];

	float widthDiv = tex0.m_fTexWidth;
	float heightDiv = tex0.m_fTexHeight;
	float t0u0;
	if( options.enableHackForGames == HACK_FOR_ALL_STAR_BASEBALL || options.enableHackForGames == HACK_FOR_MLB )
	{
		t0u0 = (fS0 -tile0.fhilite_sl);
	}
	else
	{
		t0u0 = (fS0 * tile0.fShiftScaleS -tile0.fhilite_sl);
	}
	
	float t0u1;
	if(gRDP.otherMode.cycle_type >= CYCLE_TYPE_COPY )
	{
		t0u1 = t0u0 + (fScaleS * (nX1 - nX0 - 1))*tile0.fShiftScaleS;
	}
	else
	{
		t0u1 = t0u0 + (fScaleS * (nX1 - nX0))*tile0.fShiftScaleS;
	}


	m_texRectTex1UV[0].u = t0u0/widthDiv;
	m_texRectTex1UV[1].u = t0u1/widthDiv;
	if(!tile0.bMirrorS && RemapTextureCoordinate(t0u0, t0u1, tex0.m_dwTileWidth, tile0.dwMaskS, widthDiv, m_texRectTex1UV[0].u, m_texRectTex1UV[1].u) )
		SetTextureUFlag(D3DTADDRESS_CLAMP, gRSP.curTile);

	float t0v0;
	if( options.enableHackForGames == HACK_FOR_ALL_STAR_BASEBALL || options.enableHackForGames == HACK_FOR_MLB )
	{
		t0v0 = (fT0 -tile0.fhilite_tl);
	}
	else
	{
		t0v0 = (fT0 * tile0.fShiftScaleT -tile0.fhilite_tl);
	}
	
	float t0v1;
	if (gRDP.otherMode.cycle_type >= CYCLE_TYPE_COPY)
	{
		t0v1 = t0v0 + (fScaleT * (nY1 - nY0-1))*tile0.fShiftScaleT;
	}
	else
	{
		t0v1 = t0v0 + (fScaleT * (nY1 - nY0))*tile0.fShiftScaleT;
	}

	m_texRectTex1UV[0].v = t0v0/heightDiv;
	m_texRectTex1UV[1].v = t0v1/heightDiv;
	if(!tile0.bMirrorT && RemapTextureCoordinate(t0v0, t0v1, tex0.m_dwTileHeight, tile0.dwMaskT, heightDiv, m_texRectTex1UV[0].v, m_texRectTex1UV[1].v) )
		SetTextureVFlag(D3DTADDRESS_CLAMP, gRSP.curTile);
	
	D3DCOLOR difColor;
	if( colorFlag )
		difColor = diffuseColor;
	else
		difColor = gRDP.primitiveColor;

	g_texRectTVtx[0].x = ViewPortTranslatei_x(nX0);
	g_texRectTVtx[0].y = ViewPortTranslatei_y(nY0);
	g_texRectTVtx[0].dcDiffuse = difColor;

	g_texRectTVtx[1].x = ViewPortTranslatei_x(nX1);
	g_texRectTVtx[1].y = ViewPortTranslatei_y(nY0);
	g_texRectTVtx[1].dcDiffuse = difColor;

	g_texRectTVtx[2].x = ViewPortTranslatei_x(nX1);
	g_texRectTVtx[2].y = ViewPortTranslatei_y(nY1);
	g_texRectTVtx[2].dcDiffuse = difColor;

	g_texRectTVtx[3].x = ViewPortTranslatei_x(nX0);
	g_texRectTVtx[3].y = ViewPortTranslatei_y(nY1);
	g_texRectTVtx[3].dcDiffuse = difColor;

	float depth = (gRDP.otherMode.depth_source == 1 ? gRDP.fPrimitiveDepth : 0 );

	g_texRectTVtx[0].z = g_texRectTVtx[1].z = g_texRectTVtx[2].z = g_texRectTVtx[3].z = depth;
	g_texRectTVtx[0].rhw = g_texRectTVtx[1].rhw = g_texRectTVtx[2].rhw = g_texRectTVtx[3].rhw = 1;

	if( IsTexel1Enable() )
	{
		surf = g_textures[(gRSP.curTile+1)&7].m_pCTexture;
		RenderTexture &tex1 = g_textures[(gRSP.curTile+1)&7];
		Tile &tile1 = gRDP.tiles[(gRSP.curTile+1)&7];

		widthDiv = tex1.m_fTexWidth;
		heightDiv = tex1.m_fTexHeight;
		//if( tile1.dwMaskS == 0 )	widthDiv = tile1.dwWidth;
		//if( tile1.dwMaskT == 0 )	heightDiv = tile1.dwHeight;

		float t0u0 = fS0 * tile1.fShiftScaleS -tile1.fhilite_sl;
		float t0v0 = fT0 * tile1.fShiftScaleT -tile1.fhilite_tl;
		float t0u1;
		float t0v1;
		if (gRDP.otherMode.cycle_type >= CYCLE_TYPE_COPY)
		{
			t0u1 = t0u0 + (fScaleS * (nX1 - nX0 - 1))*tile1.fShiftScaleS;
			t0v1 = t0v0 + (fScaleT * (nY1 - nY0 - 1))*tile1.fShiftScaleT;
		}
		else
		{
			t0u1 = t0u0 + (fScaleS * (nX1 - nX0))*tile1.fShiftScaleS;
			t0v1 = t0v0 + (fScaleT * (nY1 - nY0))*tile1.fShiftScaleT;
		}

		m_texRectTex2UV[0].u = t0u0/widthDiv;
		m_texRectTex2UV[1].u = t0u1/widthDiv;
		if(!tile1.bMirrorS && RemapTextureCoordinate(t0u0, t0u1, tex1.m_dwTileWidth, tile1.dwMaskS, widthDiv, m_texRectTex2UV[0].u, m_texRectTex2UV[1].u) )
			SetTextureUFlag(D3DTADDRESS_CLAMP, (gRSP.curTile+1)&7);

		m_texRectTex2UV[0].v = t0v0/heightDiv;
		m_texRectTex2UV[1].v = t0v1/heightDiv;

		if(!tile1.bMirrorT && RemapTextureCoordinate(t0v0, t0v1, tex1.m_dwTileHeight, tile1.dwMaskT, heightDiv, m_texRectTex2UV[0].v, m_texRectTex2UV[1].v) )
			SetTextureVFlag(D3DTADDRESS_CLAMP, (gRSP.curTile+1)&7);

		SetVertexTextureUVCoord(g_texRectTVtx[0], m_texRectTex1UV[0].u, m_texRectTex1UV[0].v, m_texRectTex2UV[0].u, m_texRectTex2UV[0].v);
		SetVertexTextureUVCoord(g_texRectTVtx[1], m_texRectTex1UV[1].u, m_texRectTex1UV[0].v, m_texRectTex2UV[1].u, m_texRectTex2UV[0].v);
		SetVertexTextureUVCoord(g_texRectTVtx[2], m_texRectTex1UV[1].u, m_texRectTex1UV[1].v, m_texRectTex2UV[1].u, m_texRectTex2UV[1].v);
		SetVertexTextureUVCoord(g_texRectTVtx[3], m_texRectTex1UV[0].u, m_texRectTex1UV[1].v, m_texRectTex2UV[0].u, m_texRectTex2UV[1].v);
	}
	else
	{
		SetVertexTextureUVCoord(g_texRectTVtx[0], m_texRectTex1UV[0].u, m_texRectTex1UV[0].v);
		SetVertexTextureUVCoord(g_texRectTVtx[1], m_texRectTex1UV[1].u, m_texRectTex1UV[0].v);
		SetVertexTextureUVCoord(g_texRectTVtx[2], m_texRectTex1UV[1].u, m_texRectTex1UV[1].v);
		SetVertexTextureUVCoord(g_texRectTVtx[3], m_texRectTex1UV[0].u, m_texRectTex1UV[1].v);
	}


	bool res;
	TurnFogOnOff(false);

	ApplyRDPScissor();
	res = RenderTexRect();

	TurnFogOnOff(gRDP.tnl.Fog);

	if( gRDP.otherMode.cycle_type  >= CYCLE_TYPE_COPY || !gRDP.otherMode.z_cmp  )
	{
		ZBufferEnable(gRDP.tnl.Zbuffer);
	}

	DEBUGGER_PAUSE_AT_COND_AND_DUMP_COUNT_N((eventToPause == NEXT_FLUSH_TRI || eventToPause == NEXT_TEXTRECT), {
		DebuggerAppendMsg("TexRect: tile=%d, X0=%d, Y0=%d, X1=%d, Y1=%d,\nfS0=%f, fT0=%f, ScaleS=%f, ScaleT=%f\n",
			gRSP.curTile, nX0, nY0, nX1, nY1, fS0, fT0, fScaleS, fScaleT);
		DebuggerAppendMsg("       : x0=%f, y0=%f, x1=%f, y1=%f\n",	g_texRectTVtx[0].x, g_texRectTVtx[0].y, g_texRectTVtx[2].x, g_texRectTVtx[2].y);
		DebuggerAppendMsg("   Tex0: u0=%f, v0=%f, u1=%f, v1=%f\n",	m_texRectTex1UV[0].u, m_texRectTex1UV[0].v, m_texRectTex1UV[1].u, m_texRectTex1UV[1].v);
		if( IsTexel1Enable() )
		{
			DebuggerAppendMsg("   Tex1: u0=%f, v0=%f, u1=%f, v1=%f\n",	m_texRectTex2UV[0].u, m_texRectTex2UV[0].v, m_texRectTex2UV[1].u, m_texRectTex2UV[1].v);
		}
		DebuggerAppendMsg("color=%08X, %08X\n",	g_texRectTVtx[0].dcDiffuse, g_texRectTVtx[0].dcSpecular);
		DebuggerAppendMsg("Pause after TexRect\n");
		if( logCombiners ) m_pColorCombiner->DisplayMuxString();
	});

	return res;
}


bool CRender::TexRectFlip(LONG nX0, LONG nY0, LONG nX1, LONG nY1, float fS0, float fT0, float fS1, float fT1)
{
	LOG_UCODE("TexRectFlip: X0=%d, Y0=%d, X1=%d, Y1=%d,\n\t\tfS0=%f, fT0=%f, fS1=%f, fT1=%f ",
			nX0, nY0, nX1, nY1, fS0, fT0, fS1, fT1);

	if( status.bHandleN64RenderTexture)	
	{
		status.bFrameBufferIsDrawn = true;
		status.bFrameBufferDrawnByTriangles = true;
	}

	PrepareTextures();

	if( gRDP.otherMode.depth_source == 0 )	ZBufferEnable( FALSE );

	float widthDiv = g_textures[gRSP.curTile].m_fTexWidth;
	float heightDiv = g_textures[gRSP.curTile].m_fTexHeight;

	Tile &tile0 = gRDP.tiles[gRSP.curTile];

	float t0u0 = fS0 / widthDiv;
	float t0v0 = fT0 / heightDiv;

	float t0u1 = (fS1 - fS0)/ widthDiv + t0u0;
	float t0v1 = (fT1 - fT0)/ heightDiv + t0v0;

	float depth = (gRDP.otherMode.depth_source == 1 ? gRDP.fPrimitiveDepth : 0 );

	if( t0u0 >= 0 && t0u1 <= 1 && t0u1 >= t0u0 ) SetTextureUFlag(D3DTADDRESS_CLAMP, gRSP.curTile);
	if( t0v0 >= 0 && t0v1 <= 1 && t0v1 >= t0v0 ) SetTextureVFlag(D3DTADDRESS_CLAMP, gRSP.curTile);

	SetCombinerAndBlender();

	D3DCOLOR difColor = gRDP.primitiveColor;

	// Same as TexRect, but with texcoords 0,2 swapped
	g_texRectTVtx[0].x = ViewPortTranslatei_x(nX0);
	g_texRectTVtx[0].y = ViewPortTranslatei_y(nY0);
	g_texRectTVtx[0].dcDiffuse = difColor;

	g_texRectTVtx[1].x = ViewPortTranslatei_x(nX1);
	g_texRectTVtx[1].y = ViewPortTranslatei_y(nY0);
	g_texRectTVtx[1].dcDiffuse = difColor;

	g_texRectTVtx[2].x = ViewPortTranslatei_x(nX1);
	g_texRectTVtx[2].y = ViewPortTranslatei_y(nY1);
	g_texRectTVtx[2].dcDiffuse = difColor;

	g_texRectTVtx[3].x = ViewPortTranslatei_x(nX0);
	g_texRectTVtx[3].y = ViewPortTranslatei_y(nY1);
	g_texRectTVtx[3].dcDiffuse = difColor;

	g_texRectTVtx[0].z = g_texRectTVtx[1].z = g_texRectTVtx[2].z = g_texRectTVtx[3].z = depth;
	g_texRectTVtx[0].rhw = g_texRectTVtx[1].rhw = g_texRectTVtx[2].rhw = g_texRectTVtx[3].rhw = 1.0f;
	
	SetVertexTextureUVCoord(g_texRectTVtx[0], t0u0, t0v0);
	SetVertexTextureUVCoord(g_texRectTVtx[1], t0u0, t0v1);
	SetVertexTextureUVCoord(g_texRectTVtx[2], t0u1, t0v1);
	SetVertexTextureUVCoord(g_texRectTVtx[3], t0u1, t0v0);

	TurnFogOnOff(false);
	ApplyRDPScissor();
	bool res = RenderTexRect();

	TurnFogOnOff(gRDP.tnl.Fog);

	// Restore state
	ZBufferEnable( gRDP.tnl.Zbuffer );

	DEBUGGER_PAUSE_AT_COND_AND_DUMP_COUNT_N((eventToPause == NEXT_FLUSH_TRI || eventToPause == NEXT_TEXTRECT), {
		DebuggerAppendMsg("TexRectFlip: tile=%d, X0=%d, Y0=%d, X1=%d, Y1=%d,\nfS0=%f, fT0=%f, nfS1=%f, fT1=%f\n",
			gRSP.curTile, nX0, nY0, nX1, nY1, fS0, fT0, fS1, fT1);
		DebuggerAppendMsg("       : x0=%f, y0=%f, x1=%f, y1=%f\n",	g_texRectTVtx[0].x, g_texRectTVtx[0].y, g_texRectTVtx[2].x, g_texRectTVtx[2].y);
		DebuggerAppendMsg("   Tex0: u0=%f, v0=%f, u1=%f, v1=%f\n",	g_texRectTVtx[0].tcord[0].u, g_texRectTVtx[0].tcord[0].v, g_texRectTVtx[2].tcord[0].u, g_texRectTVtx[2].tcord[0].v);
		TRACE0("Pause after TexRectFlip\n");
		if( logCombiners ) m_pColorCombiner->DisplayMuxString();
	});

	return res;
}


void CRender::StartDrawSimple2DTexture(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, D3DCOLOR dif,  float z, float rhw)
{
	g_texRectTVtx[0].x = ViewPortTranslatei_x(x0);	// << Error here, shouldn't divid by 4
	g_texRectTVtx[0].y = ViewPortTranslatei_y(y0);
	g_texRectTVtx[0].dcDiffuse = dif;
	g_texRectTVtx[0].tcord[0].u = u0;
	g_texRectTVtx[0].tcord[0].v = v0;


	g_texRectTVtx[1].x = ViewPortTranslatei_x(x1);
	g_texRectTVtx[1].y = ViewPortTranslatei_y(y0);
	g_texRectTVtx[1].dcDiffuse = dif;
	g_texRectTVtx[1].tcord[0].u = u1;
	g_texRectTVtx[1].tcord[0].v = v0;

	g_texRectTVtx[2].x = ViewPortTranslatei_x(x1);
	g_texRectTVtx[2].y = ViewPortTranslatei_y(y1);
	g_texRectTVtx[2].dcDiffuse = dif;
	g_texRectTVtx[2].tcord[0].u = u1;
	g_texRectTVtx[2].tcord[0].v = v1;

	g_texRectTVtx[3].x = ViewPortTranslatei_x(x0);
	g_texRectTVtx[3].y = ViewPortTranslatei_y(y1);
	g_texRectTVtx[3].dcDiffuse = dif;
	g_texRectTVtx[3].tcord[0].u = u0;
	g_texRectTVtx[3].tcord[0].v = v1;

	RenderTexture &txtr = g_textures[0];
	if( txtr.pTextureEntry && txtr.pTextureEntry->txtrBufIdx > 0 )
	{
		RenderTextureInfo &info = gRenderTextureInfos[txtr.pTextureEntry->txtrBufIdx-1];
		g_texRectTVtx[0].tcord[0].u *= info.scaleX;
		g_texRectTVtx[0].tcord[0].v *= info.scaleY;
		g_texRectTVtx[1].tcord[0].u *= info.scaleX;
		g_texRectTVtx[1].tcord[0].v *= info.scaleY;
		g_texRectTVtx[2].tcord[0].u *= info.scaleX;
		g_texRectTVtx[2].tcord[0].v *= info.scaleY;
		g_texRectTVtx[3].tcord[0].u *= info.scaleX;
		g_texRectTVtx[3].tcord[0].v *= info.scaleY;
	}

	g_texRectTVtx[0].z = g_texRectTVtx[1].z = g_texRectTVtx[2].z = g_texRectTVtx[3].z = z;
	g_texRectTVtx[0].rhw = g_texRectTVtx[1].rhw = g_texRectTVtx[2].rhw = g_texRectTVtx[3].rhw = rhw;
}

void CRender::StartDrawSimpleRect(LONG nX0, LONG nY0, LONG nX1, LONG nY1, uint32 dwColor, float depth, float rhw)
{
	m_simpleRectVtx[0].x = ViewPortTranslatei_x(nX0);
	m_simpleRectVtx[1].x = ViewPortTranslatei_x(nX1);
	m_simpleRectVtx[0].y = ViewPortTranslatei_y(nY0);
	m_simpleRectVtx[1].y = ViewPortTranslatei_y(nY1);
}

void CRender::SetAddressUAllStages(uint32 dwTile, int dwFlag)
{

}

void CRender::SetAddressVAllStages(uint32 dwTile, int dwFlag)
{
}

void CRender::SetAllTexelRepeatFlag()
{
	if( IsTextureEnabled() )
	{
		if( IsTexel0Enable() || gRDP.otherMode.cycle_type  == CYCLE_TYPE_COPY )
			SetTexelRepeatFlags(gRSP.curTile);
		if( IsTexel1Enable() )
			SetTexelRepeatFlags((gRSP.curTile+1)&7);
	}
}

void CRender::SetTexelRepeatFlags(uint32 dwTile)
{
	Tile &tile = gRDP.tiles[dwTile];

	if( tile.bForceClampS )
		SetTextureUFlag(D3DTADDRESS_CLAMP, dwTile);
	else if( tile.bForceWrapS )
			SetTextureUFlag(D3DTADDRESS_WRAP, dwTile);
	else if( tile.dwMaskS == 0 || tile.bClampS )
	{
		if( gRDP.otherMode.cycle_type  >= CYCLE_TYPE_COPY )
			SetTextureUFlag(D3DTADDRESS_WRAP, dwTile);	// Can not clamp in COPY/FILL mode
		else
			SetTextureUFlag(D3DTADDRESS_CLAMP, dwTile);
	}
	else if (tile.bMirrorS )
		SetTextureUFlag(D3DTADDRESS_MIRROR, dwTile);
	else								
		SetTextureUFlag(D3DTADDRESS_WRAP, dwTile);
	
	if( tile.bForceClampT )
		SetTextureVFlag(D3DTADDRESS_CLAMP, dwTile);
	else if( tile.bForceWrapT )
		SetTextureVFlag(D3DTADDRESS_WRAP, dwTile);
	else if( tile.dwMaskT == 0 || tile.bClampT)
	{
		if( gRDP.otherMode.cycle_type  >= CYCLE_TYPE_COPY )
			SetTextureVFlag(D3DTADDRESS_WRAP, dwTile);	// Can not clamp in COPY/FILL mode
		else
			SetTextureVFlag(D3DTADDRESS_CLAMP, dwTile);
	}
	else if (tile.bMirrorT )
		SetTextureVFlag(D3DTADDRESS_MIRROR, dwTile);
	else								
		SetTextureVFlag(D3DTADDRESS_WRAP, dwTile);
}

void CRender::Initialize(void)
{
	ClearDeviceObjects();
	InitDeviceObjects();
}

void CRender::CleanUp(void)
{
	m_pColorCombiner->CleanUp();
	ClearDeviceObjects();
}

void CRender::SetTextureEnable(bool bEnable)
{
	gRSP.bTextureEnabled = bEnable;
}

void CRender::SetTextureScale(int dwTile,  float fScaleX, float fScaleY)
{
	if( gRSP.bTextureEnabled )
	{
		if( gRSP.curTile != dwTile )
			gRDP.textureIsChanged = true;

		gRSP.curTile = dwTile;

		gRSP.fTexScaleX = fScaleX;
		gRSP.fTexScaleY = fScaleY;

		if( fScaleX == 0 || fScaleY == 0 )
		{
			gRSP.fTexScaleX = 1/32.0f;
			gRSP.fTexScaleY = 1/32.0f;
		}
	}	
}

void CRender::SetViewport(int nLeft, int nTop, int nRight, int nBottom, int maxZ)
{
	if( status.bHandleN64RenderTexture )
		return;

	static float MultX=0, MultY=0;

	if( gRSP.nVPLeftN == nLeft && gRSP.nVPTopN == nTop &&
		gRSP.nVPRightN == nRight && gRSP.nVPBottomN == nBottom &&
		MultX==windowSetting.fMultX && MultY==windowSetting.fMultY)
	{
		// no changes
		return;
	}

	MultX=windowSetting.fMultX;
	MultY=windowSetting.fMultY;

	gRSP.maxZ = maxZ;
	gRSP.nVPLeftN = nLeft;
	gRSP.nVPTopN = nTop;
	gRSP.nVPRightN = nRight;
	gRSP.nVPBottomN = nBottom;
	gRSP.nVPWidthN = nRight - nLeft + 1;
	gRSP.nVPHeightN = nBottom - nTop + 1;

	UpdateClipRectangle();

	LOG_UCODE("SetViewport (%d,%d - %d,%d)",gRSP.nVPLeftN, gRSP.nVPTopN, gRSP.nVPRightN, gRSP.nVPBottomN);
}

bool CRender::DrawTriangles()
{
	if( !status.bCIBufferIsRendered ) g_pFrameBufferManager->ActiveTextureBuffer();

	DEBUGGER_ONLY_IF( (!debuggerEnableZBuffer), {ZBufferEnable( FALSE );} );

	// Hack for Pilotwings 64 (U) [!].v64
	static bool skipNext=false;
	if( options.enableHackForGames == HACK_FOR_PILOT_WINGS )
	{
		if( IsUsedAsDI(g_CI.dwAddr) && gRDP.otherMode.z_cmp+gRDP.otherMode.z_upd > 0 )
		{
			TRACE0("Warning: using Flushtris to write Zbuffer" );
			gRSP.numVertices = 0;
			gRSP.maxVertexID = 0;
			skipNext = true;
			return true;
		}
		else if( skipNext )
		{
			skipNext = false;
			gRSP.numVertices = 0;
			gRSP.maxVertexID = 0;
			return true;
		}	
	}

	if( status.bN64IsDrawingTextureBuffer && frameBufferOptions.bIgnore )
	{
		gRSP.numVertices = 0;
		gRSP.maxVertexID = 0;
		return true;
	}

	extern bool bConkerHideShadow;
	if( options.enableHackForGames == HACK_FOR_CONKER && bConkerHideShadow )
	{
		gRSP.numVertices = 0;
		gRSP.maxVertexID = 0;
		return true;
	}

	if( IsUsedAsDI(g_CI.dwAddr) && !status.bHandleN64RenderTexture )
	{
		status.bFrameBufferIsDrawn = true;
	}

	if (gRSP.numVertices == 0)
		return true;
	if( status.bHandleN64RenderTexture )
	{
		g_pRenderTextureInfo->maxUsedHeight = g_pRenderTextureInfo->N64Height;
		status.bFrameBufferIsDrawn = true;
		status.bFrameBufferDrawnByTriangles = true;
	}

	if( !gRDP.bFogEnableInBlender && gRDP.tnl.Fog )
	{
		TurnFogOnOff(false);
	}

	for( int t=0; t<2; t++ )
	{
		float halfscaleS = 1;
		float halfscaleT = 1;

		// This will get rid of the thin black lines
		if( t==0 && !(m_pColorCombiner->m_bTex0Enabled) ) 
			continue;
		else
		{
			if( ( gRDP.tiles[gRSP.curTile].dwSize == TXT_SIZE_32b && options.enableHackForGames == HACK_FOR_RUMBLE ) ||
				(options.enableHackForGames == HACK_FOR_POLARISSNOCROSS && gRDP.tiles[7].dwFormat == TXT_FMT_CI && gRDP.tiles[7].dwSize == TXT_SIZE_8b 
				&& gRDP.tiles[0].dwFormat == TXT_FMT_CI && gRDP.tiles[0].dwSize == TXT_SIZE_8b && gRSP.curTile == 0 ))
			{
				halfscaleS = 0.5;
				halfscaleT = 0.5;
			}
		}

		if( t==1 && !(m_pColorCombiner->m_bTex1Enabled) ) break;

		uint32 i;
		if( halfscaleS < 1 )
		{
			for( i=0; i<gRSP.numVertices; i++ )
			{
				if( t == 0 )
				{
					g_vtxBuffer[i].tcord[t].u += gRSP.tex0OffsetX;
					g_vtxBuffer[i].tcord[t].u /= 2;
					g_vtxBuffer[i].tcord[t].u -= gRSP.tex0OffsetX;
					g_vtxBuffer[i].tcord[t].v += gRSP.tex0OffsetY;
					g_vtxBuffer[i].tcord[t].v /= 2;
					g_vtxBuffer[i].tcord[t].v -= gRSP.tex0OffsetY;
				}
				else
				{
					g_vtxBuffer[i].tcord[t].u += gRSP.tex1OffsetX;
					g_vtxBuffer[i].tcord[t].u /= 2;
					g_vtxBuffer[i].tcord[t].u -= gRSP.tex1OffsetX;
					g_vtxBuffer[i].tcord[t].v += gRSP.tex1OffsetY;
					g_vtxBuffer[i].tcord[t].v /= 2;
					g_vtxBuffer[i].tcord[t].v -= gRSP.tex1OffsetY;
				}
			}
		}
	}

	if( status.bHandleN64RenderTexture && g_pRenderTextureInfo->CI_Info.dwSize == TXT_SIZE_8b )
	{
		ZBufferEnable(FALSE);
	}

	ApplyScissorWithClipRatio();

	if( g_curRomInfo.bZHack )
	{
		extern void HackZAll();
		HackZAll();
	}

	bool res = RenderFlushTris();
	g_clippedVtxCount = 0;

	LOG_UCODE("DrawTriangles: Draw %d Triangles", gRSP.numVertices/3);
	
	gRSP.numVertices = 0;	// Reset index
	gRSP.maxVertexID = 0;

	DEBUGGER_PAUSE_AND_DUMP_COUNT_N(NEXT_FLUSH_TRI, {
		TRACE0("Pause after DrawTriangles\n");
		if( logCombiners ) m_pColorCombiner->DisplayMuxString();
	});

	if( !gRDP.bFogEnableInBlender && gRDP.tnl.Fog )
	{
		TurnFogOnOff(true);
	}

	return res;
}

#ifdef _DEBUG
bool CRender::DrawTexture(int tex, TextureChannel channel)
{
	if( g_textures[tex].m_pCTexture == NULL )
	{
		TRACE0("Can't draw null texture");
		return false;
	}

	//REPLACEMESaveTextureToFile(tex, channel, true);	// Save to file instead of draw to screen

	DebuggerAppendMsg("Texture %d (CurTile:%d): W=%f, H=%f, Real W=%d, H=%d", tex, gRSP.curTile, 
		g_textures[tex].m_fTexWidth, g_textures[tex].m_fTexHeight, g_textures[tex].m_dwTileWidth, g_textures[tex].m_dwTileHeight);
	DebuggerAppendMsg("X scale: %f, Y scale: %f, %s", gRSP.fTexScaleX, gRSP.fTexScaleY, gRSP.bTextureEnabled?"Enabled":"Disabled");

	return true;
}
#endif

extern RenderTextureInfo gRenderTextureInfos[];
void SetVertexTextureUVCoord(TexCord &dst, float s, float t, int tile, TxtrCacheEntry *pEntry)
{
	RenderTexture &txtr = g_textures[tile];
	RenderTextureInfo &info = gRenderTextureInfos[pEntry->txtrBufIdx-1];

	uint32 addrOffset = g_TI.dwAddr-info.CI_Info.dwAddr;
	uint32 extraTop = (addrOffset>>(info.CI_Info.dwSize-1)) /info.CI_Info.dwWidth;
	uint32 extraLeft = (addrOffset>>(info.CI_Info.dwSize-1))%info.CI_Info.dwWidth;

	if( pEntry->txtrBufIdx > 0  )
	{
		// Loading from render_texture or back buffer
		s += (extraLeft+pEntry->ti.LeftToLoad)/txtr.m_fTexWidth;
		t += (extraTop+pEntry->ti.TopToLoad)/txtr.m_fTexHeight;

		s *= info.scaleX;
		t *= info.scaleY;
	}

	dst.u = s;
	dst.v = t;
}

void CRender::SetVertexTextureUVCoord(TLITVERTEX &v, float fTex0S, float fTex0T)
{
	RenderTexture &txtr = g_textures[0];
	if( txtr.pTextureEntry && txtr.pTextureEntry->txtrBufIdx > 0 )
	{
		::SetVertexTextureUVCoord(v.tcord[0], fTex0S, fTex0T, 0, txtr.pTextureEntry);
	}
	else
	{
		v.tcord[0].u = fTex0S;
		v.tcord[0].v = fTex0T;
	}
}
void CRender::SetVertexTextureUVCoord(TLITVERTEX &v, float fTex0S, float fTex0T, float fTex1S, float fTex1T)
{
	if( (options.enableHackForGames == HACK_FOR_ZELDA||options.enableHackForGames == HACK_FOR_ZELDA_MM) && m_Mux == 0x00262a60150c937f && gRSP.curTile == 0 )
	{
		// Hack for Zelda Sun
		Tile &t0 = gRDP.tiles[0];
		Tile &t1 = gRDP.tiles[1];
		if( t0.dwFormat == TXT_FMT_I && t0.dwSize == TXT_SIZE_8b && t0.dwWidth == 64 &&
			t1.dwFormat == TXT_FMT_I && t1.dwSize == TXT_SIZE_8b && t1.dwWidth == 64 &&
			t0.dwHeight == t1.dwHeight )
		{
			fTex0S /= 2;
			fTex0T /= 2;
			fTex1S /= 2;
			fTex1T /= 2;
		}
	}

	RenderTexture &txtr0 = g_textures[0];
	if( txtr0.pTextureEntry && txtr0.pTextureEntry->txtrBufIdx > 0 )
	{
		::SetVertexTextureUVCoord(v.tcord[0], fTex0S, fTex0T, 0, txtr0.pTextureEntry);
	}
	else
	{
		v.tcord[0].u = fTex0S;
		v.tcord[0].v = fTex0T;
	}

	RenderTexture &txtr1 = g_textures[1];
	if( txtr1.pTextureEntry && txtr1.pTextureEntry->txtrBufIdx > 0 )
	{
		::SetVertexTextureUVCoord(v.tcord[1], fTex1S, fTex1T, 1, txtr1.pTextureEntry);
	}
	else
	{
		v.tcord[1].u = fTex1S;
		v.tcord[1].v = fTex1T;
	}
}

void CRender::UpdateClipRectangle()
{
	if( status.bHandleN64RenderTexture )
	{
		//windowSetting.fMultX = windowSetting.fMultY = 1;
		windowSetting.vpLeftW = 0;
		windowSetting.vpTopW = 0;
		windowSetting.vpRightW = newRenderTextureInfo.bufferWidth;
		windowSetting.vpBottomW = newRenderTextureInfo.bufferHeight;
		windowSetting.vpWidthW = newRenderTextureInfo.bufferWidth;
		windowSetting.vpHeightW = newRenderTextureInfo.bufferHeight;

		gRSP.vtxXMul = windowSetting.vpWidthW/2.0f;
		gRSP.vtxXAdd = gRSP.vtxXMul + windowSetting.vpLeftW;
		gRSP.vtxYMul = -windowSetting.vpHeightW/2.0f;
		gRSP.vtxYAdd = windowSetting.vpHeightW/2.0f + windowSetting.vpTopW;

		// Update clip rectangle by setting scissor

		int halfx = newRenderTextureInfo.bufferWidth/2;
		int halfy = newRenderTextureInfo.bufferHeight/2;
		int centerx = halfx;
		int centery = halfy;

		gRSP.clip_ratio_left = centerx - halfx;
		gRSP.clip_ratio_top = centery - halfy;
		gRSP.clip_ratio_right = centerx + halfx;
		gRSP.clip_ratio_bottom = centery + halfy;
	}
	else
	{
		windowSetting.vpLeftW = int(gRSP.nVPLeftN * windowSetting.fMultX);
		windowSetting.vpTopW = int(gRSP.nVPTopN  * windowSetting.fMultY);
		windowSetting.vpRightW = int(gRSP.nVPRightN* windowSetting.fMultX);
		windowSetting.vpBottomW = int(gRSP.nVPBottomN* windowSetting.fMultY);
		windowSetting.vpWidthW = int((gRSP.nVPRightN - gRSP.nVPLeftN + 1) * windowSetting.fMultX);
		windowSetting.vpHeightW = int((gRSP.nVPBottomN - gRSP.nVPTopN + 1) * windowSetting.fMultY);

		gRSP.vtxXMul = windowSetting.vpWidthW/2.0f;
		gRSP.vtxXAdd = gRSP.vtxXMul + windowSetting.vpLeftW;
		gRSP.vtxYMul = -windowSetting.vpHeightW/2.0f;
		gRSP.vtxYAdd = windowSetting.vpHeightW/2.0f + windowSetting.vpTopW;

		// Update clip rectangle by setting scissor

		int halfx = gRSP.nVPWidthN/2;
		int halfy = gRSP.nVPHeightN/2;
		int centerx = gRSP.nVPLeftN+halfx;
		int centery = gRSP.nVPTopN+halfy;

		gRSP.clip_ratio_left = centerx - halfx;
		gRSP.clip_ratio_top = centery - halfy;
		gRSP.clip_ratio_right = centerx + halfx;
		gRSP.clip_ratio_bottom = centery + halfy;
	}


	UpdateScissorWithClipRatio();
}

void CRender::UpdateScissorWithClipRatio()
{
	gRSP.real_clip_scissor_left = max(gRDP.scissor.left, gRSP.clip_ratio_left);
	gRSP.real_clip_scissor_top = max(gRDP.scissor.top, gRSP.clip_ratio_top);
	gRSP.real_clip_scissor_right = min(gRDP.scissor.right,gRSP.clip_ratio_right);
	gRSP.real_clip_scissor_bottom = min(gRDP.scissor.bottom, gRSP.clip_ratio_bottom);

	gRSP.real_clip_scissor_left = max(gRSP.real_clip_scissor_left, 0);
	gRSP.real_clip_scissor_top = max(gRSP.real_clip_scissor_top, 0);
	gRSP.real_clip_scissor_right = min(gRSP.real_clip_scissor_right,windowSetting.uViWidth-1);
	gRSP.real_clip_scissor_bottom = min(gRSP.real_clip_scissor_bottom, windowSetting.uViHeight-1);

	WindowSettingStruct &w = windowSetting;
	w.clipping.left = (uint32)(gRSP.real_clip_scissor_left*windowSetting.fMultX);
	w.clipping.top	= (uint32)(gRSP.real_clip_scissor_top*windowSetting.fMultY);
	w.clipping.bottom = (uint32)(gRSP.real_clip_scissor_bottom*windowSetting.fMultY);
	w.clipping.right = (uint32)(gRSP.real_clip_scissor_right*windowSetting.fMultX);
	if( w.clipping.left > 0 || w.clipping.top > 0 || w.clipping.right < (uint32)windowSetting.uDisplayWidth-1 ||
		w.clipping.bottom < (uint32)windowSetting.uDisplayHeight-1 )
	{
		w.clipping.needToClip = true;
	}
	else
	{
		w.clipping.needToClip = false;
	}
	w.clipping.width  = (uint32)max((gRSP.real_clip_scissor_right - gRSP.real_clip_scissor_left + 1)*windowSetting.fMultX, 0.0f);
	w.clipping.height = (uint32)max((gRSP.real_clip_scissor_bottom - gRSP.real_clip_scissor_top + 1)*windowSetting.fMultY, 0.0f);

	float halfx = gRSP.nVPWidthN/2.0f;
	float halfy = gRSP.nVPHeightN/2.0f;
	float centerx = gRSP.nVPLeftN+halfx;
	float centery = gRSP.nVPTopN+halfy;

	ApplyScissorWithClipRatio(true);
}


//MOVE ME//REPLACE ME//WE SHOULD DO THIS ELSEWHERE
void CRender::InitOtherModes(void)					// Set other modes not covered by color combiner or alpha blender
{

	if (gRDP.otherMode.text_filt != 0) //If not point
	{
		CRender::g_pRender->SetTextureFilter(RDP_TFILTER_BILERP);
	}
	else
	{
		CRender::g_pRender->SetTextureFilter(RDP_TFILTER_POINT);
	}

	if (gRDP.otherMode.zmode == 3)
		CRender::g_pRender->SetZBias(2);
	else
		CRender::g_pRender->SetZBias(0);


	if ((gRDP.tnl.Zbuffer & gRDP.otherMode.z_cmp) | gRDP.otherMode.z_upd)
	{
		CRender::g_pRender->SetZCompare(true);
	}
	else
	{
		CRender::g_pRender->SetZCompare(false);
	}

	CRender::g_pRender->SetZUpdate(gRDP.otherMode.z_upd ? true : false);

	if ((gRDP.otherMode.alpha_compare == 1) && !gRDP.otherMode.alpha_cvg_sel)
	{
		ForceAlphaRef(m_dwAlpha);
		SetAlphaTestEnable(TRUE);
	}
	else if (gRDP.otherMode.cvg_x_alpha)
	{
		ForceAlphaRef(128);
		SetAlphaTestEnable(TRUE);
	}
	else
	{
		SetAlphaTestEnable(FALSE);
	}

	uint16 blender = gRDP.otherMode.blender;
	RDP_BlenderSetting &bl = *(RDP_BlenderSetting*)(&(blender));
	if (bl.c1_m1a == 3 || bl.c1_m2a == 3 || bl.c2_m1a == 3 || bl.c2_m2a == 3)
		gRDP.bFogEnableInBlender = true;
	else
		gRDP.bFogEnableInBlender = false;
}


void CRender::SetTextureFilter(uint32 dwFilter)
{
	switch( options.forceTextureFilter )
	{
		case FORCE_DEFAULT_FILTER:
			switch(dwFilter)
			{
				case RDP_TFILTER_AVERAGE:	//?
				case RDP_TFILTER_BILERP:
					m_dwMinFilter = m_dwMagFilter = D3DTEXF_LINEAR;
					break;
				default:
					m_dwMinFilter = m_dwMagFilter = D3DTEXF_POINT;
					break;
			}
			break;
		case FORCE_POINT_FILTER:
			m_dwMinFilter = m_dwMagFilter = D3DTEXF_POINT;
			break;
		case FORCE_LINEAR_FILTER:
		case FORCE_BILINEAR_FILTER:
			m_dwMinFilter = m_dwMagFilter = D3DTEXF_LINEAR;
			break;
	}

	ApplyTextureFilter();
}

void CRender::SaveTextureToFile(CTexture &texture, char *filename, int width, int height)
{
	if (width < 0 || height < 0)
	{
		width = texture.m_dwWidth;
		height = texture.m_dwHeight;
	}

	BYTE *pbuf = new BYTE[width*height*4];
	if (pbuf)
	{
		DrawInfo srcInfo;
		if (texture.StartUpdate(&srcInfo))
		{
			uint32 *pbuf2 = (uint32*)pbuf;
			for (int i = height - 1; i >= 0; i--)
			{
				uint32 *pSrc = (uint32*)((BYTE*)srcInfo.lpSurface + srcInfo.lPitch * i);
				for (int j = 0; j<width; j++)
				{
					*pbuf2++ = *pSrc++;
				}
			}

			SaveRGBABufferToPNGFile(filename, (BYTE*)pbuf, width, height);
			texture.EndUpdate(&srcInfo);
		}
		else
		{
			TRACE0("Cannot lock texture");
		}
		delete[] pbuf;
	}
	else
	{
		TRACE0("Out of memory");
	}
}

bool SaveRGBABufferToPNGFile(char *filename, unsigned char *buf, int width, int height)
{

	if (_stricmp(right(filename, 4), ".png") != 0)	strcat(filename, ".png");

	struct BMGImageStruct img;
	InitBMGImage(&img);
	img.bits = buf;
	img.bits_per_pixel = 32;
	img.height = height;
	img.width = width;
	img.scan_width = width * 4;
	BMG_Error code = WritePNG(filename, img);

	if (code == BMG_OK)
		return true;
	else
		return false;
}