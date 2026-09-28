/*************************************************************************************************\
gameTacMap.cpp			: Implementation of the gameTacMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"gametacmap.h"
#include"team.h"
#include"comndr.h"
#include"objmgr.h"
#include"cellip.h"
#include"gamecam.h"
#include"objective.h"
#include"mission.h"
#include"platform_windows.h"
#include "../resource.h"
extern unsigned char godMode;
extern bool useLeftRightMouseProfile;

#define SQUARE_BLIP 0
#define DOT_BLIP 1
#define TRIANGLE_BLIP 2

const float GameTacMap::s_blinkLength = .5f;
float		GameTacMap::s_lastBlinkTime = 0.f;

extern bool ShowMovers;

GameTacMap::GameTacMap()
{
	top = 0;
	left = 0;
	right = 0;
	bottom = 0;

	buildingPoints = NULL;
	buildingCount = 0;

	navMarkerCount = 0;
	curNavMarker = -1;

	objectiveAnimationId = -1;
	objectiveFlashTime = 0.0f;
	objectiveNumFlashes = 0;
}

void GameTacMap::init( unsigned char* bitmapData, int dataSize )
{

	EllipseElement::init();
	TGAFileHeader* pHeader = (TGAFileHeader*)bitmapData;

	bmpWidth = pHeader->width;
	bmpHeight = pHeader->height;

	textureHandle = gos_NewTextureFromMemory(gos_Texture_Solid,".tga",bitmapData,dataSize,0);
	char path[256];
	strcpy(  path, artPath );
	strcat( path, "viewingrect.tga" );
	viewRectHandle = mcTextureManager->loadTexture(path, gos_Texture_Alpha, 0 );

	strcpy( path, artPath );
	strcat( path, "blip.tga" );
	blipHandle = mcTextureManager->loadTexture( path, gos_Texture_Alpha, 0 );
}

void GameTacMap::update()
{
	Stuff::Vector2DOf<long> screen;
	// The tac map is drawn through the bottom-band HUD batch and is shrunk by
	// s_hud_scale (flushHUDBatch, single bottom-center anchor) during a mission,
	// exactly like the command bar and force-group bar. Its hit-rect
	// (left/top/right/bottom, from rectInfos[0].rect) and the tacMapToWorld
	// size math are authored at 100% coords, so the click must be mapped back
	// through the HUD-shrink inverse -- mirror the force-group bar
	// (forcegroupbar.cpp) which uses getMouseHudX/Y. Using raw getMouseX/Y here
	// made the authored rect bleed upward over the bottom-left buttons (click =
	// camera jump to top of map) and offset/mis-scaled genuine minimap clicks.
	screen.x = userInput->getMouseHudX();
	screen.y = userInput->getMouseHudY();

	float width = right - left;
	float height = bottom - top;


	if ( !inRegion(screen.x, screen.y) )
		return;

	ControlGui::instance->setRolloverHelpText( IDS_TACMAP_HELP );
	
	if ( userInput->isLeftClick() )	
	{
		screen.x -= left;
		screen.y -= top;
		
		Stuff::Vector3D world;

		tacMapToWorld( screen, width, height, world );
		if (MissionInterfaceManager::instance()->getControlGui()->isAddingAirstrike() &&  
			MissionInterfaceManager::instance()->getControlGui()->isButtonPressed( ControlGui::SENSOR_PROBE ))
			MissionInterfaceManager::instance()->doMove(world);
		else
		{
			eye->setPosition( world, false );
			((GameCamera*)(eye))->setTarget( 0 );
		}
	}
	else if ( userInput->isRightClick() && useLeftRightMouseProfile )
	{
		screen.x -= left;
		screen.y -= top;
		
		Stuff::Vector3D world;

		tacMapToWorld( screen, width, height, world );

		MissionInterfaceManager::instance()->doMove(world);
	}

}

bool GameTacMap::animate (long objectiveId, long nFlashes)
{
	if (objectiveAnimationId == -1)
	{
		objectiveAnimationId = objectiveId - 1;
		objectiveFlashTime = 0.0f;
		objectiveNumFlashes = nFlashes;

		return true;
	}

	return false;
}

void GameTacMap::render()
{
	if (turn < 2)		//Terrain not setup yet.  Left,Right,Top,Bottom are poopy!
		return;

	gos_VERTEX corners[5];

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero );

	gos_SetRenderState( gos_State_Specular,FALSE );
	gos_SetRenderState( gos_State_AlphaTest, FALSE );
			
	gos_SetRenderState( gos_State_Filter, gos_FilterNone );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_Texture, textureHandle );


	for ( int i = 0; i < 4; ++i )
	{
		corners[i].rhw = 1.0f;
		corners[i].argb = 0xffffffff;
		corners[i].frgb = 0;
		corners[i].z = 0.0f;
	}


	corners[0].x = corners[2].x = left;
	corners[1].x = corners[3].x = right;
	corners[3].y = corners[2].y = bottom;
	corners[0].y = corners[1].y = top;

	corners[0].u = 2.f/(float)bmpWidth;
	corners[3].u = 128.f/(float)bmpWidth;
	corners[2].u = 2.f/(float)bmpWidth;
	corners[1].u = 128.f/(float)bmpWidth;
	corners[0].v = 2.f/(float)bmpWidth;
	corners[3].v = 128.f/(float)bmpHeight;
	corners[2].v = 128.f/(float)bmpHeight;
	corners[1].v = 2.f/(float)bmpWidth;
	
	gos_DrawTriangles( corners, 3 );
	gos_DrawTriangles( &corners[1], 3 );


	//-----------------------------------------------------------
	// Render the objective markers
	long count = 0;
	for ( EList< CObjective*, CObjective* >::EIterator iter =  Team::home->objectives.Begin();
		!iter.IsDone(); iter++ )
	{
		//We are there.  Start flashing.
		if ((objectiveAnimationId == count) && objectiveNumFlashes)
		{
			objectiveFlashTime += frameLength;
			if ( objectiveFlashTime > .5f )
			{
				objectiveFlashTime = 0.0f;
				objectiveNumFlashes--;
			}
			else if ( objectiveFlashTime > 0.25f)
			{
				(*iter)->RenderMarkers(this, 0);
			}

			if (objectiveNumFlashes == 0)
			{
				//Flashing is done.  We now return you to your regularly scheduled program.
				objectiveAnimationId = 0;
			}
		}
		else
		{
			(*iter)->RenderMarkers(this, 0);
		}

		count++;
	}

	// The little yellow viewing rect. Retail inverse-projected four screen
	// points with eye->inverseProjectZ, which the port retired. Instead, solve
	// each screen corner against the ground plane with the forward
	// world-to-clip matrix the GPU renders with.
	// ponytail: flat ground plane at the camera focus elevation; hills tilt
	// the real footprint a little, raycast the heightfield if that shows.
	bool viewRectOk = (eye != NULL && land != NULL);
	if ( viewRectOk )
	{
		const Stuff::Matrix4D M = eye->worldToClipGL();
		Stuff::Vector3D focus = eye->getPosition();
		const float h = land->getTerrainElevation( focus );
		float vmx, vmy, vax, vay;
		gos_GetViewport( &vmx, &vmy, &vax, &vay );

		// Same screen points as retail: top edge and 2/3 down, left and right.
		const float px[4] = { vax + 1.f, vax + 1.f, vax + vmx - 1.f, vax + vmx - 1.f };
		const float py[4] = { vay + 1.f, vay + vmy * 0.6667f - 1.f, vay + vmy * 0.6667f - 1.f, vay + 1.f };
		for ( int c = 0; c < 4 && viewRectOk; ++c )
		{
			const float nx = 2.f * (px[c] - vax) / vmx - 1.f;
			const float ny = 1.f - 2.f * (py[c] - vay) / vmy;
			// clip = X*M(0,.) + Y*M(1,.) + h*M(2,.) + M(3,.); ndc = clip.xy / clip.w
			const float a1 = M(0,0) - nx * M(0,3), b1 = M(1,0) - nx * M(1,3);
			const float c1 = -( h * (M(2,0) - nx * M(2,3)) + M(3,0) - nx * M(3,3) );
			const float a2 = M(0,1) - ny * M(0,3), b2 = M(1,1) - ny * M(1,3);
			const float c2 = -( h * (M(2,1) - ny * M(2,3)) + M(3,1) - ny * M(3,3) );
			const float det = a1 * b2 - a2 * b1;
			if ( fabs( det ) < 1e-12f ) { viewRectOk = false; break; }
			Stuff::Vector3D world;
			world.x = (c1 * b2 - c2 * b1) / det;
			world.y = (a1 * c2 - a2 * c1) / det;
			world.z = h;
			// Corner above the horizon: the ray never reaches the ground.
			const float w = world.x * M(0,3) + world.y * M(1,3) + h * M(2,3) + M(3,3);
			if ( w <= 0.f ) { viewRectOk = false; break; }
			worldToTacMap( world, corners[c] );
		}
	}

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
	gos_SetRenderState( gos_State_MonoEnable, 0);
	gos_SetRenderState( gos_State_Perspective, 0);
	gos_SetRenderState( gos_State_Clipping, 2);
	gos_SetRenderState( gos_State_AlphaTest, 0);
	gos_SetRenderState( gos_State_Specular, 0);
	gos_SetRenderState( gos_State_Dither, 1);
	gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
	gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
	gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_ZWrite, 0);

	if ( viewRectOk )
	{
		for ( int c = 0; c < 4; ++c )
		{
			corners[c].argb = 0xffffffff;
			corners[c].frgb = 0;
			corners[c].rhw = 1.0f;
			corners[c].z = 0.0f;
		}
		corners[0].u = corners[1].u = 0.078125f;
		corners[3].u = corners[2].u = .99875f;
		corners[0].v = corners[3].v = 0.078125f;
		corners[1].v = corners[2].v = .99875f;
		gos_SetRenderState( gos_State_Texture, mcTextureManager->get_gosTextureHandle( viewRectHandle ) );
		gos_DrawQuads( &corners[0], 4 );
	}

	unsigned long colors[MAX_MOVERS];
	unsigned long ringColors[MAX_MOVERS];
	Stuff::Vector3D positions[MAX_MOVERS];
	unsigned long ranges[MAX_MOVERS];
	bool		  selected[MAX_MOVERS];

	count = 0;

	//------------------------------------------------------------
	// draw non-movers, must do separate check for vehicles, I'm not sure they
	// have sensors
	for (int i = 0; i < MAX_TEAMS; ++i )
	{
		TeamSensorSystem* pSys = SensorManager->getTeamSensor( i );
		
		if ( pSys )
		{
			for ( int j = 0; j < pSys->numSensors; ++j )
			{
				SensorSystem* pSensor = pSys->sensors[j];

				if ( !pSensor )
					continue;

				if ( pSensor->owner->isDestroyed() || pSensor->owner->isDisabled() || pSensor->owner->getStatus() == OBJECT_STATUS_SHUTDOWN )
					continue;

				if ( pSensor->getRange() < 1.1 || pSensor->broken)
					continue;

				if (!pSensor->owner->getTeam())
					continue;

				ObjectClass objClass = pSensor->owner->getObjectClass();

				unsigned long colorBlip = pSensor->owner->getSelected() ? 0xff4bff4b : 0xff00cc00;
				unsigned long colorRing = 0xff00cc00;

			
				if ( pSensor->owner->getTeam()->isNeutral( Team::home ) )
				{
					colorBlip = pSensor->owner->getSelected() ? 0xff4c4cff : 0xff0000ff;
					colorRing = 0xff0000ff;
				}
				else if ( pSensor->owner->getTeam()->isEnemy( Team::home ) ) // enemy
				{
					
					{
						colorBlip = pSensor->owner->getSelected() ? 0xffff3f3f : 0xffff0000;
						colorRing = 0xffff0000;
					}
				}

				if ( objClass != BATTLEMECH && objClass != GROUNDVEHICLE)
				{
					if ( objClass == ARTILLERY )
					{
						// blink
						s_lastBlinkTime += frameLength;
						if ( s_lastBlinkTime > s_blinkLength )
						{
							colorBlip = 0;
							colorRing = 0;
							s_lastBlinkTime = 0.f;
						}

					}

					colors[count] = colorBlip;
					ringColors[count] = colorRing;
					ranges[count] = pSensor->getRange();
					selected[count] = 0;
					positions[count] = pSensor->owner->getPosition();
					count++;

				}
			}
		}
	}

	unsigned long colorBlip, colorRing;

	//-----------------------------------------------------	
	// draw the movers
	for (int i=0;i<(ObjectManager->numMovers);i++)
	{
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover && mover->getExists() && !(mover->isDestroyed() || mover->isDisabled()))
		{
			SensorSystem* pSensor = mover->getSensorSystem();
			float range = pSensor ? pSensor->getRange() : 0;
			long contactStatus = mover->getContactStatus(Team::home->getId(), true);
			if (mover->getTeamId() == Team::home->id)
			{
				if (mover->getCommanderId() == Commander::home->getId())
				{
					if (mover->isOnGUI())
					{
						colorBlip = mover->getSelected() ? 0xff4bff4b : 0xff00cc00;
						mover->getStatus() == OBJECT_STATUS_SHUTDOWN ? colorRing = 0 : colorRing =  0xff00cc00;
					}
					else
						continue;
				}
				else
				{
					if (mover->isOnGUI() && land->IsGameSelectTerrainPosition(mover->getPosition()) && mover->pathLocks)
					{
						colorBlip = mover->getSelected() ? 0xff4b4bff : 0xff0000cc;
						mover->getStatus() == OBJECT_STATUS_SHUTDOWN ? colorRing = 0 : colorRing =  0xff0000cc;
					}
					else
						continue;
				}
			}
			else if (ShowMovers || (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]) || ((mover->getTeamId() != Team::home->id)
				&&  ( contactStatus != CONTACT_NONE )
				&&  (mover->getStatus() != OBJECT_STATUS_SHUTDOWN) 
				&&  (!mover->hasNullSignature())
				&&	(mover->getEcmRange() <= 0.0f) ) )	//Do not draw ECM mechs!!)
			{
				//Not on our side.  Draw by contact status
				colorBlip = mover->getSelected() ? 0xffff3f3f : 0xffff0000;
				colorRing = 0xffff0000;
				
			}
			else
				continue;

			colors[count] = colorBlip;
			ringColors[count] = colorRing;
			ranges[count] = range;
			selected[count] = mover->getSelected();
			positions[count] = mover->getPosition();
			count++;

		}
	}

	for (int i = 0; i < count; i++ )
	{
		drawSensor( positions[i], ranges[i], ringColors[i] );
	}
	bool bSel = 0; // draw unselected first
	for ( int j = 0; j < 2; j++ )
	{
		for ( int i = 0; i < count; i++ )
		{
			if ( selected[i] == bSel )
			{
				drawBlip( positions[i], colors[i], DOT_BLIP );
			}
		}
		bSel = 1;
	}



}

void GameTacMap::worldToTacMap( Stuff::Vector3D& world, gos_VERTEX& tac )
{
	TacMap::worldToTacMap( world, left, top, right - left, bottom - top, tac );
}	
void GameTacMap::initBuildings( unsigned char* data, int size )
{
	if ( data && size >= (int)sizeof(int32_t) )
	{
		int32_t* pData = (int32_t*)data;
		buildingCount = *pData++;

		// Bound the count by what the packet actually holds (2 int32s per
		// building); a corrupt/short-read packet must not drive the walk
		// below off the end of the buffer.
		const int32_t maxCount = (size - (int)sizeof(int32_t)) / (2 * (int)sizeof(int32_t));
		if ( buildingCount < 0 || buildingCount > maxCount )
		{
			printf("[TACMAP] initBuildings: buildingCount=%d exceeds packet capacity %d (size=%d) -- clamping to 0\n",
				(int)buildingCount, (int)maxCount, size);
			buildingCount = 0;
		}

		if (buildingCount)
		{
			buildingPoints = new gos_VERTEX[buildingCount];
			gos_VERTEX* pTmp = buildingPoints;

			for ( int i = 0; i < buildingCount; ++i, pTmp++)
			{
				pTmp->x = *pData++ + left;
				pTmp->y = *pData++ + top;
				pTmp->z = 0;
				pTmp->argb = BUILDING_COLOR;
				pTmp->frgb = 0;
				pTmp->rhw = .5;
				pTmp->u = 0.f;
				pTmp->v = 0.f;
			}
		}
	}
}

void GameTacMap::drawSensor( const Stuff::Vector3D& pos, float radius, long color )
{
	if ( color == 0 )
		return;
	gos_VERTEX sqare[4];

	if ( radius > 1 )
		radius += land->metersPerCell * 6.f; // a little fudge

	radius *= ((float)(right - left))/(land->metersPerCell * land->realVerticesMapSide * 3.f);

	for ( int i = 0; i < 4; ++i )
	{
		sqare[i].z = 0;
		sqare[i].rhw = .5;
		sqare[i].argb = color;
		sqare[i].frgb = 0;
	}

	worldToTacMap( (Stuff::Vector3D&)pos, sqare[0] );

	sqare[1].x = sqare[0].x;
	sqare[1].y = sqare[0].y + 1;

	sqare[2].x = sqare[0].x + 1;
	sqare[2].y = sqare[0].y;

	sqare[3].x = sqare[0].x + 1;
	sqare[3].y = sqare[0].y + 1;

	// need to draw that round thing
	Stuff::Vector2DOf< long > center;
	Stuff::Vector2DOf< long > radii;
	center.x = float2long(sqare[1].x);
	center.y = float2long(sqare[1].y);
	radii.x = 2.0f * float2long(radius+.5);
	radii.y = 2.0f * float2long(radius+.5);


	EllipseElement circle( center, radii, color, 0 ); 
	GUI_RECT rect = { left, top, right, bottom };
	circle.setClip( rect );

	circle.draw();


}

void GameTacMap::drawBlip( const Stuff::Vector3D& pos, long color, int type )
{
	if ( color == 0 )
		return;
	gos_VERTEX triangle[4];

	gos_SetRenderState( gos_State_AlphaTest, 1);
	gos_SetRenderState( gos_State_Specular, 0);
	gos_SetRenderState( gos_State_Dither, 1);
	gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState(	gos_State_ZWrite, 0);
	unsigned long gosID = mcTextureManager->get_gosTextureHandle( blipHandle );
	gos_SetRenderState( gos_State_Texture, gosID );

	for ( int i = 0; i < 4; ++i )
	{
		triangle[i].z = 0;
		triangle[i].rhw = .5;
		triangle[i].argb = color;
		triangle[i].frgb = 0;
		triangle[i].u = 0.f;
		triangle[i].v = 0.f;
	}

	

	worldToTacMap( (Stuff::Vector3D&)pos, triangle[0] );
	triangle[0].x -= 2.f;
	triangle[1].x = triangle[0].x;
	triangle[2].x = triangle[3].x = triangle[0].x + 4.1f;
	triangle[0].y -= 2.f;
	triangle[3].y = triangle[0].y;
	triangle[1].y = triangle[2].y = triangle[0].y + 4.1f;
	triangle[2].u = triangle[3].u = .250001f;
	triangle[1].v = triangle[2].v = .250001f;

	gos_DrawQuads( triangle, 4 );
}

void GameTacMap::setPos( const GUI_RECT& newPos )
{
	left = newPos.left;
	right = newPos.right;
	top = newPos.top;
	bottom = newPos.bottom;
}



//*************************************************************************************************
// end of file ( TacMap.cpp )

//*************************************************************************************************
// end of file ( gameTacMap.cpp )
