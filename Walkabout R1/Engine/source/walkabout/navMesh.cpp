//
// T3D 1.1 NavMesh class and supporting methods.
// Copyright (c) Daniel Buckmaster, 2012
//

#include <stdio.h>

#include "navMesh.h"
#include "navMeshDraw.h"
#include "navContext.h"
#include "recast/DetourDebugDraw.h"
#include "recast/RecastDebugDraw.h"

#include "math/mathUtils.h"
#include "math/mRandom.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/typeValidators.h"

#include "scene/sceneRenderState.h"
#include "gfx/gfxDrawUtil.h"
#include "renderInstance/renderPassManager.h"
#include "gfx/primBuilder.h"

#include "core/stream/bitStream.h"
#include "math/mathIO.h"

extern bool gEditingMission;

namespace Nav {
   /* ------------------------------------------------------------
       ----------------------- NavMesh ------------------------- */

   IMPLEMENT_CO_NETOBJECT_V1(NavMesh);

   const U32 NavMesh::mMaxVertsPerPoly = 3;

   SimObjectPtr<SimSet> NavMesh::smServerSet = NULL;

   SimSet *NavMesh::getServerSet()
   {
      if(!smServerSet)
      {
         SimSet *set = NULL;
         if(Sim::findObject("ServerNavMeshSet", set))
            smServerSet = set;
         else
         {
            smServerSet = new SimSet();
            smServerSet->registerObject("ServerNavMeshSet");
            Sim::getRootGroup()->addObject(smServerSet);
         }
      }
      return smServerSet;
   }

   SimObjectPtr<EventManager> NavMesh::smEventManager = NULL;

   EventManager *NavMesh::getEventManager()
   {
      if(!smEventManager)
      {
         smEventManager = new EventManager();
         smEventManager->registerObject("NavEventManager");
         Sim::getRootGroup()->addObject(smEventManager);
         smEventManager->setMessageQueue("NavEventManagerQueue");
         smEventManager->registerEvent("NavMeshCreated");
         smEventManager->registerEvent("NavMeshRemoved");
         smEventManager->registerEvent("NavMeshStartUpdate");
         smEventManager->registerEvent("NavMeshUpdate");
         smEventManager->registerEvent("NavMeshBuildTime");
         smEventManager->registerEvent("NavMeshTileUpdate");
         smEventManager->registerEvent("NavMeshUpdateBox");
         smEventManager->registerEvent("NavMeshObstacleAdded");
         smEventManager->registerEvent("NavMeshObstacleRemoved");
      }
      return smEventManager;
   }

   NavMesh::NavMesh()
   {
      mTypeMask |= MarkerObjectType;
      mFileName = StringTable->insert("");
      mNetFlags.clear(Ghostable);

      mBackgroundBuild = true;

      mSaveIntermediates = true;
      nm = NULL;
      ctx = NULL;

      dMemset(&cfg, 0, sizeof(cfg));
      mCellSize = mCellHeight = 0.2f;
      mWalkableHeight = 2.0f;
      mWalkableClimb = 0.3f;
      mWalkableRadius = 0.5f;
      mWalkableSlope = 40.0f;
      mBorderSize = 1;
      mDetailSampleDist = 6.0f;
      mDetailSampleMaxError = 1.0f;
      mMaxEdgeLen = 12;
      mMaxSimplificationError = 1.3f;
      mMinRegionArea = 8;
      mMergeRegionArea = 20;
      mTileSize = 10.0f;
      mMaxTiles = 128;
      mMaxPolysPerTile = 128;

      mSmallCharacters = false;
      mRegularCharacters = true;
      mLargeCharacters = false;
      mVehicles = false;

      mCoverSet = StringTable->insert("");
      mInnerCover = false;
      mCoverDist = 1.0f;
      mPeekDist = 0.7f;

      mBuilding = false;
   }

   NavMesh::~NavMesh()
   {
      dtFreeNavMesh(nm);
      nm = NULL;
      delete ctx;
      ctx = NULL;
   }

   bool NavMesh::setProtectedDetailSampleDist(void *obj, const char *index, const char *data)
   {
      F32 dist = dAtof(data);
      if(dist == 0.0f || dist >= 0.9f)
         return true;
      Con::errorf("NavMesh::detailSampleDist must be 0 or greater than 0.9!");
      return false;
   }

   FRangeValidator ValidCellSize(0.01f, 10.0f);
   FRangeValidator ValidSlopeAngle(0.0f, 89.9f);
   IRangeValidator PositiveInt(0, S32_MAX);
   IRangeValidator NaturalNumber(1, S32_MAX);
   FRangeValidator CornerAngle(0.0f, 90.0f);

   void NavMesh::initPersistFields()
   {
      addGroup("NavMesh Build");

      addField("saveIntermediates", TypeBool, Offset(mSaveIntermediates, NavMesh),
         "Store intermediate results for debug rendering.");

      addField("backgroundBuild", TypeBool, Offset(mBackgroundBuild, NavMesh),
         "Build NavMesh in the background (slower).");

      endGroup("NavMesh Build");

      addGroup("NavMesh Options");

      addField("fileName", TypeString, Offset(mFileName, NavMesh),
         "Name of the data file to store this navmesh in (relative to engine executable).");

      addFieldV("cellSize", TypeF32, Offset(mCellSize, NavMesh), &ValidCellSize,
         "Length/width of a voxel.");
      addFieldV("cellHeight", TypeF32, Offset(mCellHeight, NavMesh), &ValidCellSize,
         "Height of a voxel.");

      addFieldV("actorHeight", TypeF32, Offset(mWalkableHeight, NavMesh), &CommonValidators::PositiveFloat,
         "Height of an actor.");
      addFieldV("actorClimb", TypeF32, Offset(mWalkableClimb, NavMesh), &CommonValidators::PositiveFloat,
         "Maximum climbing height of an actor.");
      addFieldV("actorRadius", TypeF32, Offset(mWalkableRadius, NavMesh), &CommonValidators::PositiveFloat,
         "Radius of an actor.");
      addFieldV("walkableSlope", TypeF32, Offset(mWalkableSlope, NavMesh), &ValidSlopeAngle,
         "Maximum walkable slope in degrees.");

      addField("smallCharacters", TypeBool, Offset(mSmallCharacters, NavMesh),
         "Is this NavMesh for smaller-than-usual characters?");
      addField("regularCharacters", TypeBool, Offset(mRegularCharacters, NavMesh),
         "Is this NavMesh for regular-sized characters?");
      addField("largeCharacters", TypeBool, Offset(mLargeCharacters, NavMesh),
         "Is this NavMesh for larger-than-usual characters?");
      addField("vehicles", TypeBool, Offset(mVehicles, NavMesh),
         "Is this NavMesh for characters driving vehicles?");

      endGroup("NavMesh Options");

      addGroup("NavMesh Annotations");

      addField("coverGroup", TypeString, Offset(mCoverSet, NavMesh),
         "Name of the SimGroup to store cover points in.");

      addField("innerCover", TypeBool, Offset(mInnerCover, NavMesh),
         "Add cover points everywhere, not just on corners?");

      addField("coverDist", TypeF32, Offset(mCoverDist, NavMesh),
         "Distance from the edge of the NavMesh to search for cover.");
      addField("peekDist", TypeF32, Offset(mPeekDist, NavMesh),
         "Distance to the side of each cover point that peeking happens.");

      endGroup("NavMesh Annotations");

      addGroup("NavMesh Advanced Options");

      addFieldV("borderSize", TypeS32, Offset(mBorderSize, NavMesh), &PositiveInt,
         "Size of the non-walkable border around the navigation mesh (in voxels).");
      addProtectedField("detailSampleDist", TypeF32, Offset(mDetailSampleDist, NavMesh),
         &setProtectedDetailSampleDist, &defaultProtectedGetFn,
         "Sets the sampling distance to use when generating the detail mesh.");
      addFieldV("detailSampleError", TypeF32, Offset(mDetailSampleMaxError, NavMesh), &CommonValidators::PositiveFloat,
         "The maximum distance the detail mesh surface should deviate from heightfield data.");
      addFieldV("maxEdgeLen", TypeS32, Offset(mDetailSampleDist, NavMesh), &PositiveInt,
         "The maximum allowed length for contour edges along the border of the mesh.");
      addFieldV("simplificationError", TypeF32, Offset(mMaxSimplificationError, NavMesh), &CommonValidators::PositiveFloat,
         "The maximum distance a simplfied contour's border edges should deviate from the original raw contour.");
      addFieldV("minRegionArea", TypeS32, Offset(mMinRegionArea, NavMesh), &PositiveInt,
         "The minimum number of cells allowed to form isolated island areas.");
      addFieldV("mergeRegionArea", TypeS32, Offset(mMergeRegionArea, NavMesh), &PositiveInt,
         "Any regions with a span count smaller than this value will, if possible, be merged with larger regions.");
      addFieldV("tileSize", TypeF32, Offset(mTileSize, NavMesh), &CommonValidators::PositiveNonZeroFloat,
         "The horizontal size of tiles.");
      addFieldV("maxTiles", TypeS32, Offset(mMaxTiles, NavMesh), &NaturalNumber,
         "The maximum number of tiles allowed in the mesh.");
      addFieldV("maxPolysPerTile", TypeS32, Offset(mMaxPolysPerTile, NavMesh), &NaturalNumber,
         "The maximum number of polygons allowed in a tile.");

      endGroup("NavMesh Advanced Options");

      Parent::initPersistFields();
   }

   bool NavMesh::onAdd()
   {
      if(!Parent::onAdd())
         return false;

      mObjBox.set(Point3F(-1.0f, -1.0f, -1.0f),
         Point3F( 1.0f,  1.0f,  1.0f));
      resetWorldBox();

      addToScene();

      if(gEditingMission)
         mNetFlags.set(Ghostable);

      if(isServerObject())
      {
#ifdef WALKABOUT_DEMO
         if(getServerSet()->size() >= 2)
         {
            Con::executef("OnWalkaboutDemoLimit");
            return false;
         }
#endif
         getServerSet()->addObject(this);
         ctx = new NavContext();
         setProcessTick(true);
         if(getEventManager())
            getEventManager()->postEvent("NavMeshCreated", getIdString());
      }

      load();

      return true;
   }

   void NavMesh::onRemove()
   {
      if(getEventManager())
         getEventManager()->postEvent("NavMeshRemoved", getIdString());

      removeFromScene();

      Parent::onRemove();
   }

   void NavMesh::setTransform(const MatrixF &mat)
   {
      Parent::setTransform(mat);
   }

   void NavMesh::setScale(const VectorF &scale)
   {
      Parent::setScale(scale);
   }

   void NavMesh::addLink(const Point3F &from, const Point3F &to)
   {
      Point3F rcFrom = DTStoRC(from), rcTo = DTStoRC(to);
      mLinkVerts.push_back(rcFrom.x);
      mLinkVerts.push_back(rcFrom.y);
      mLinkVerts.push_back(rcFrom.z);
      mLinkVerts.push_back(rcTo.x);
      mLinkVerts.push_back(rcTo.y);
      mLinkVerts.push_back(rcTo.z);
      mLinksUnsynced.push_back(true);
      mLinkRads.push_back(mWalkableRadius);
      mLinkDirs.push_back(0);
      mLinkAreas.push_back(OffMeshArea);
      {
         Point3F dir = to - from;
         F32 drop = -dir.z;
         dir.z = 0;
         // If we drop more than we travel horizontally, we're a drop link.
         if(drop > dir.len())
            mLinkFlags.push_back(DropFlag);
         else
            mLinkFlags.push_back(JumpFlag);
      }
      mLinkIDs.push_back(1000 + mCurLinkID);
      mLinkSelectStates.push_back(Unselected);
      mDeleteLinks.push_back(false);
      mCurLinkID++;
   }

   S32 NavMesh::getLink(const Point3F &pos)
   {
      for(U32 i = 0; i < mLinkIDs.size(); i++)
      {
         if(mDeleteLinks[i])
            continue;
         SphereF start(getLinkStart(i), mLinkRads[i]);
         SphereF end(getLinkEnd(i), mLinkRads[i]);
         if(start.isContained(pos) || end.isContained(pos))
            return i;
      }
      return -1;
   }

   LinkData NavMesh::getLinkData(U32 idx)
   {
      if(idx < mLinkIDs.size())
      {
         return LinkData(mLinkFlags[idx]);
      }
      return LinkData();
   }

   void NavMesh::setLinkData(U32 idx, const LinkData &d)
   {
      if(idx < mLinkIDs.size())
      {
         mLinkFlags[idx] = d.getFlags();
         mLinksUnsynced[idx] = true;
      }
   }

   Point3F NavMesh::getLinkStart(U32 idx)
   {
      return RCtoDTS(Point3F(
         mLinkVerts[idx*6],
         mLinkVerts[idx*6 + 1],
         mLinkVerts[idx*6 + 2]));
   }

   Point3F NavMesh::getLinkEnd(U32 idx)
   {
      return RCtoDTS(Point3F(
         mLinkVerts[idx*6 + 3],
         mLinkVerts[idx*6 + 4],
         mLinkVerts[idx*6 + 5]));
   }

   void NavMesh::selectLink(U32 idx, bool select, bool hover)
   {
      if(idx < mLinkIDs.size())
      {
         if(!select)
            mLinkSelectStates[idx] = Unselected;
         else
            mLinkSelectStates[idx] = hover ? Hovered : Selected;
      }
   }

   void NavMesh::eraseLink(U32 i)
   {
      mLinkVerts.erase(i*6, 6);
      mLinksUnsynced.erase(i);
      mLinkRads.erase(i);
      mLinkDirs.erase(i);
      mLinkAreas.erase(i);
      mLinkFlags.erase(i);
      mLinkIDs.erase(i);
      mLinkSelectStates.erase(i);
      mDeleteLinks.erase(i);
   }

   void NavMesh::eraseLinks()
   {
      mLinkVerts.clear();
      mLinksUnsynced.clear();
      mLinkRads.clear();
      mLinkDirs.clear();
      mLinkAreas.clear();
      mLinkFlags.clear();
      mLinkIDs.clear();
      mLinkSelectStates.clear();
      mDeleteLinks.clear();
   }

   void NavMesh::setLinkCount(U32 c)
   {
      eraseLinks();
      mLinkVerts.setSize(c * 6);
      mLinksUnsynced.setSize(c);
      mLinkRads.setSize(c);
      mLinkDirs.setSize(c);
      mLinkAreas.setSize(c);
      mLinkFlags.setSize(c);
      mLinkIDs.setSize(c);
      mLinkSelectStates.setSize(c);
      mDeleteLinks.setSize(c);
   }

   void NavMesh::deleteLink(U32 idx)
   {
      if(idx < mLinkIDs.size())
      {
         mDeleteLinks[idx] = true;
         if(mLinksUnsynced[idx])
            eraseLink(idx);
         else
            mLinksUnsynced[idx] = true;
      }
   }

   bool NavMesh::build()
   {
      if(mBuilding)
         cancelBuild();
      else
      {
         if(getEventManager())
            getEventManager()->postEvent("NavMeshStartUpdate", getIdString());
      }

      mBuilding = true;

      ctx->startTimer(RC_TIMER_TOTAL);

      dtFreeNavMesh(nm);
      // Allocate a new navmesh.
      nm = dtAllocNavMesh();
      if(!nm)
      {
         Con::errorf("Could not allocate dtNavMesh for NavMesh %s", getIdString());
         return false;
      }

      updateConfig();

      // Build navmesh parameters from console members.
      dtNavMeshParams params;
      rcVcopy(params.orig, cfg.bmin);
      params.tileWidth = cfg.tileSize * mCellSize;
      params.tileHeight = cfg.tileSize * mCellSize;
      params.maxTiles = mMaxTiles;
      params.maxPolys = mMaxPolysPerTile;

      // Initialise our navmesh.
      if(dtStatusFailed(nm->init(&params)))
      {
         Con::errorf("Could not init dtNavMesh for NavMesh %s", getIdString());
         return false;
      }

      // Update links to be deleted.
      for(U32 i = 0; i < mLinkIDs.size();)
      {
         if(mDeleteLinks[i])
            eraseLink(i);
         else
            i++;
      }
      mLinksUnsynced.fill(false);
      mCurLinkID = 0;

      updateTiles(true);

      return true;
   }

   void NavMesh::cancelBuild()
   {
      while(mDirtyTiles.size()) mDirtyTiles.pop();
      ctx->stopTimer(RC_TIMER_TOTAL);
      mBuilding = false;
   }

   void NavMesh::inspectPostApply()
   {
      if(mBuilding)
         cancelBuild();
   }

   void NavMesh::updateConfig()
   {
      // Build rcConfig object from our console members.
      dMemset(&cfg, 0, sizeof(cfg));
      cfg.cs = mCellSize;
      cfg.ch = mCellHeight;
      Box3F box = DTStoRC(getWorldBox());
      rcVcopy(cfg.bmin, box.minExtents);
      rcVcopy(cfg.bmax, box.maxExtents);
      rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

      cfg.walkableHeight = mCeil(mWalkableHeight / mCellHeight);
      cfg.walkableClimb = mCeil(mWalkableClimb / mCellHeight);
      cfg.walkableRadius = mCeil(mWalkableRadius / mCellSize);
      cfg.walkableSlopeAngle = mWalkableSlope;
      cfg.borderSize = cfg.walkableRadius + 3;

      cfg.detailSampleDist = mDetailSampleDist;
      cfg.detailSampleMaxError = mDetailSampleMaxError;
      cfg.maxEdgeLen = mMaxEdgeLen;
      cfg.maxSimplificationError = mMaxSimplificationError;
      cfg.maxVertsPerPoly = mMaxVertsPerPoly;
      cfg.minRegionArea = mMinRegionArea;
      cfg.mergeRegionArea = mMergeRegionArea;
      cfg.tileSize = mTileSize / cfg.cs;
   }

   S32 NavMesh::getTile(Point3F pos)
   {
      if(mBuilding)
         return -1;
      for(U32 i = 0; i < mTiles.size(); i++)
      {
         if(mTiles[i].box.isContained(pos))
            return i;
      }
      return -1;
   }

   Box3F NavMesh::getTileBox(U32 id)
   {
      if(mBuilding || id >= mTiles.size())
         return Box3F::Invalid;
      return mTiles[id].box;
   }

   void NavMesh::updateTiles(bool dirty)
   {
      if(!isProperlyAdded())
         return;

      mTiles.clear();
      mTileData.clear();
      while(mDirtyTiles.size()) mDirtyTiles.pop();

      const Box3F &box = DTStoRC(getWorldBox());
      if(box.isEmpty())
         return;

      updateConfig();

      // Calculate tile dimensions.
      const U32 ts = cfg.tileSize;
      const U32 tw = (cfg.width  + ts-1) / ts;
      const U32 th = (cfg.height + ts-1) / ts;
      const F32 tcs = cfg.tileSize * cfg.cs;

      // Iterate over tiles.
      F32 tileBmin[3], tileBmax[3];
      for(U32 y = 0; y < th; ++y)
      {
         for(U32 x = 0; x < tw; ++x)
         {
            tileBmin[0] = cfg.bmin[0] + x*tcs;
            tileBmin[1] = cfg.bmin[1];
            tileBmin[2] = cfg.bmin[2] + y*tcs;

            tileBmax[0] = cfg.bmin[0] + (x+1)*tcs;
            tileBmax[1] = cfg.bmax[1];
            tileBmax[2] = cfg.bmin[2] + (y+1)*tcs;

            mTiles.push_back(
               Tile(RCtoDTS(tileBmin, tileBmax),
                    x, y,
                    tileBmin, tileBmax));

            if(dirty)
               mDirtyTiles.push(mTiles.size() - 1);

            if(mSaveIntermediates)
               mTileData.increment();
         }
      }
   }

   void NavMesh::processTick(const Move *move)
   {
      buildNextTile();
   }

   void NavMesh::buildNextTile()
   {
      if(mDirtyTiles.size())
      {
         // Pop a single dirty tile and process it.
         U32 i = mDirtyTiles.front();
         mDirtyTiles.pop();
         const Tile &tile = mTiles[i];
         // Intermediate data for tile build.
         TileData tempdata;
         TileData &tdata = mSaveIntermediates ? mTileData[i] : tempdata;
         // Generate navmesh for this tile.
         U32 dataSize = 0;
         unsigned char* data = buildTileData(tile, tdata, dataSize);
         if(data)
         {
            // Remove any previous data.
            nm->removeTile(nm->getTileRefAt(tile.x, tile.y, 0), 0, 0);
            // Add new data (navmesh owns and deletes the data).
            dtStatus status = nm->addTile(data, dataSize, DT_TILE_FREE_DATA, 0, 0);
            int success = 1;
            if(dtStatusFailed(status))
            {
               success = 0;
               //Con::errorf("Failed to add tile (%d, %d) to dtNavMesh for NavMesh %s",
                  //tile.x, tile.y, getIdString());
               dtFree(data);
            }
            if(getEventManager())
            {
               String str = String::ToString("%d (%d, %d) %d %s", getId(), tile.x, tile.y, success, castConsoleTypeToString(tile.box));
               getEventManager()->postEvent("NavMeshTileUpdate", str.c_str());
            }
         }
         // Did we just build the last tile?
         if(!mDirtyTiles.size())
         {
            ctx->stopTimer(RC_TIMER_TOTAL);
            if(getEventManager())
            {
               String str = String::ToString("%d %.3fs", getId(), ctx->getAccumulatedTime(RC_TIMER_TOTAL) / 1000.0f);
               getEventManager()->postEvent("NavMeshBuildTime", str.c_str());
               getEventManager()->postEvent("NavMeshUpdate", getIdString());
            }
            mBuilding = false;
         }
      }
   }

   static void buildCallback(SceneObject* object,void *key)
   {
      SceneContainer::CallbackInfo* info = reinterpret_cast<SceneContainer::CallbackInfo*>(key);
      object->buildPolyList(info->context,info->polyList,info->boundingBox,info->boundingSphere);
   }

   unsigned char *NavMesh::buildTileData(const Tile &tile, TileData &data, U32 &dataSize)
   {
      // Free all data that may be hanging around from last build.
      data.freeAll();

      // Push out tile boundaries a bit.
      F32 tileBmin[3], tileBmax[3];
      rcVcopy(tileBmin, tile.bmin);
      rcVcopy(tileBmax, tile.bmax);
      tileBmin[0] -= cfg.borderSize * cfg.cs;
      tileBmin[2] -= cfg.borderSize * cfg.cs;
      tileBmax[0] += cfg.borderSize * cfg.cs;
      tileBmax[2] += cfg.borderSize * cfg.cs;

      // Parse objects from level into RC-compatible format.
      Box3F box = RCtoDTS(tileBmin, tileBmax);
      SceneContainer::CallbackInfo info;
      info.context = PLC_Navigation;
      info.boundingBox = box;
      info.polyList = &data.geom;
      getContainer()->findObjects(box, StaticObjectType, buildCallback, &info);

      // Check for no geometry.
      if(!data.geom.getVertCount())
         return false;

      // Figure out voxel dimensions of this tile.
      U32 width = 0, height = 0;
      width = cfg.tileSize + cfg.borderSize * 2;
      height = cfg.tileSize + cfg.borderSize * 2;

      // Create a heightfield to voxelise our input geometry.
      data.hf = rcAllocHeightfield();
      if(!data.hf)
      {
         Con::errorf("Out of memory (rcHeightField) for NavMesh %s", getIdString());
         return NULL;
      }
      if(!rcCreateHeightfield(ctx, *data.hf, width, height, tileBmin, tileBmax, cfg.cs, cfg.ch))
      {
         Con::errorf("Could not generate rcHeightField for NavMesh %s", getIdString());
         return NULL;
      }

      unsigned char *areas = new unsigned char[data.geom.getTriCount()];
      if(!areas)
      {
         Con::errorf("Out of memory (area flags) for NavMesh %s", getIdString());
         return NULL;
      }
      dMemset(areas, 0, data.geom.getTriCount() * sizeof(unsigned char));

      // Filter triangles by angle and rasterize.
      rcMarkWalkableTriangles(ctx, cfg.walkableSlopeAngle,
         data.geom.getVerts(), data.geom.getVertCount(),
         data.geom.getTris(), data.geom.getTriCount(), areas);
      rcRasterizeTriangles(ctx, data.geom.getVerts(), data.geom.getVertCount(),
         data.geom.getTris(), areas, data.geom.getTriCount(),
         *data.hf, cfg.walkableClimb);

      delete[] areas;

      // Filter out areas with low ceilings and other stuff.
      rcFilterLowHangingWalkableObstacles(ctx, cfg.walkableClimb, *data.hf);
      rcFilterLedgeSpans(ctx, cfg.walkableHeight, cfg.walkableClimb, *data.hf);
      rcFilterWalkableLowHeightSpans(ctx, cfg.walkableHeight, *data.hf);

      data.chf = rcAllocCompactHeightfield();
      if(!data.chf)
      {
         Con::errorf("Out of memory (rcCompactHeightField) for NavMesh %s", getIdString());
         return NULL;
      }
      if(!rcBuildCompactHeightfield(ctx, cfg.walkableHeight, cfg.walkableClimb, *data.hf, *data.chf))
      {
         Con::errorf("Could not generate rcCompactHeightField for NavMesh %s", getIdString());
         return NULL;
      }
      if(!rcErodeWalkableArea(ctx, cfg.walkableRadius, *data.chf))
      {
         Con::errorf("Could not erode walkable area for NavMesh %s", getIdString());
         return NULL;
      }

      //--------------------------
      // Todo: mark areas here.
      //const ConvexVolume* vols = m_geom->getConvexVolumes();
      //for (int i  = 0; i < m_geom->getConvexVolumeCount(); ++i)
         //rcMarkConvexPolyArea(m_ctx, vols[i].verts, vols[i].nverts, vols[i].hmin, vols[i].hmax, (unsigned char)vols[i].area, *m_chf);
      //--------------------------

      if(false)
      {
         if(!rcBuildRegionsMonotone(ctx, *data.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
         {
            Con::errorf("Could not build regions for NavMesh %s", getIdString());
            return NULL;
         }
      }
      else
      {
         if(!rcBuildDistanceField(ctx, *data.chf))
         {
            Con::errorf("Could not build distance field for NavMesh %s", getIdString());
            return NULL;
         }
         if(!rcBuildRegions(ctx, *data.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
         {
            Con::errorf("Could not build regions for NavMesh %s", getIdString());
            return NULL;
         }
      }

      data.cs = rcAllocContourSet();
      if(!data.cs)
      {
         Con::errorf("Out of memory (rcContourSet) for NavMesh %s", getIdString());
         return NULL;
      }
      if(!rcBuildContours(ctx, *data.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *data.cs))
      {
         Con::errorf("Could not construct rcContourSet for NavMesh %s", getIdString());
         return NULL;
      }
      if(data.cs->nconts <= 0)
      {
         Con::errorf("No contours in rcContourSet for NavMesh %s", getIdString());
         return NULL;
      }

      data.pm = rcAllocPolyMesh();
      if(!data.pm)
      {
         Con::errorf("Out of memory (rcPolyMesh) for NavMesh %s", getIdString());
         return NULL;
      }
      if(!rcBuildPolyMesh(ctx, *data.cs, cfg.maxVertsPerPoly, *data.pm))
      {
         Con::errorf("Could not construct rcPolyMesh for NavMesh %s", getIdString());
         return NULL;
      }

      data.pmd = rcAllocPolyMeshDetail();
      if(!data.pmd)
      {
         Con::errorf("Out of memory (rcPolyMeshDetail) for NavMesh %s", getIdString());
         return NULL;
      }
      if(!rcBuildPolyMeshDetail(ctx, *data.pm, *data.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *data.pmd))
      {
         Con::errorf("Could not construct rcPolyMeshDetail for NavMesh %s", getIdString());
         return NULL;
      }

      if(data.pm->nverts >= 0xffff)
      {
         Con::errorf("Too many vertices in rcPolyMesh for NavMesh %s", getIdString());
         return NULL;
      }
      for(U32 i = 0; i < data.pm->npolys; i++)
      {
         if(data.pm->areas[i] == RC_WALKABLE_AREA)
            data.pm->areas[i] = GroundArea;

         if(data.pm->areas[i] == GroundArea)
            data.pm->flags[i] |= WalkFlag;
         if(data.pm->areas[i] == WaterArea)
            data.pm->flags[i] |= SwimFlag;
      }

      unsigned char* navData = 0;
      int navDataSize = 0;

      dtNavMeshCreateParams params;
      dMemset(&params, 0, sizeof(params));

      params.verts = data.pm->verts;
      params.vertCount = data.pm->nverts;
      params.polys = data.pm->polys;
      params.polyAreas = data.pm->areas;
      params.polyFlags = data.pm->flags;
      params.polyCount = data.pm->npolys;
      params.nvp = data.pm->nvp;

      params.detailMeshes = data.pmd->meshes;
      params.detailVerts = data.pmd->verts;
      params.detailVertsCount = data.pmd->nverts;
      params.detailTris = data.pmd->tris;
      params.detailTriCount = data.pmd->ntris;

      params.offMeshConVerts = mLinkVerts.address();
      params.offMeshConRad = mLinkRads.address();
      params.offMeshConDir = mLinkDirs.address();
      params.offMeshConAreas = mLinkAreas.address();
      params.offMeshConFlags = mLinkFlags.address();
      params.offMeshConUserID = mLinkIDs.address();
      params.offMeshConCount = mLinkIDs.size();

      params.walkableHeight = mWalkableHeight;
      params.walkableRadius = mWalkableRadius;
      params.walkableClimb = mWalkableClimb;
      params.tileX = tile.x;
      params.tileY = tile.y;
      params.tileLayer = 0;
      rcVcopy(params.bmin, data.pm->bmin);
      rcVcopy(params.bmax, data.pm->bmax);
      params.cs = cfg.cs;
      params.ch = cfg.ch;
      params.buildBvTree = true;

      if(!dtCreateNavMeshData(&params, &navData, &navDataSize))
      {
         Con::errorf("Could not create dtNavMeshData for tile (%d, %d) of NavMesh %s",
            tile.x, tile.y, getIdString());
         return NULL;
      }

      dataSize = navDataSize;

      return navData;
   }

   /// This method should never be called in a separate thread to the rendering
   /// or pathfinding logic. It directly replaces data in the dtNavMesh for
   /// this NavMesh object.
   void NavMesh::buildTiles(const Box3F &box)
   {
      // Make sure we've already built or loaded.
      if(!nm)
         return;
      // Iterate over tiles.
      for(U32 i = 0; i < mTiles.size(); i++)
      {
         const Tile &tile = mTiles[i];
         // Check tile box.
         if(!tile.box.isOverlapped(box))
            continue;
         // Mark as dirty.
         mDirtyTiles.push(i);
      }
      if(mDirtyTiles.size())
         ctx->startTimer(RC_TIMER_TOTAL);
   }

   void NavMesh::buildTile(const U32 &tile)
   {
      if(tile < mTiles.size())
      {
         mDirtyTiles.push(tile);
         ctx->startTimer(RC_TIMER_TOTAL);
      }
   }

   void NavMesh::buildLinks()
   {
      // Make sure we've already built or loaded.
      if(!nm)
         return;
      // Iterate over tiles.
      for(U32 i = 0; i < mTiles.size(); i++)
      {
         const Tile &tile = mTiles[i];
         // Iterate over links
         for(U32 j = 0; j < mLinkIDs.size(); j++)
         {
            if(tile.box.isContained(getLinkStart(j)) ||
               tile.box.isContained(getLinkEnd(j)) &&
               mLinksUnsynced[j])
            {
               // Mark tile for build.
               mDirtyTiles.push(i);
               // Delete link if necessary
               if(mDeleteLinks[j])
               {
                  eraseLink(j);
                  j--;
               }
               else
                  mLinksUnsynced[j] = false;
            }
         }
      }
      if(mDirtyTiles.size())
         ctx->startTimer(RC_TIMER_TOTAL);
   }

   void NavMesh::deleteCoverPoints()
   {
      SimSet *set = NULL;
      if(Sim::findObject(mCoverSet, set))
         set->deleteAllObjects();
   }

   bool NavMesh::createCoverPoints()
   {
      if(!nm || !isServerObject())
         return false;

      SimSet *set = NULL;
      if(Sim::findObject(mCoverSet, set))
      {
         set->deleteAllObjects();
      }
      else
      {
         set = new SimGroup();
         if(set->registerObject(mCoverSet))
         {
            getGroup()->addObject(set);
         }
         else
         {
            delete set;
            set = getGroup();
         }
      }

      dtNavMeshQuery *query = dtAllocNavMeshQuery();
      if(!query || dtStatusFailed(query->init(nm, 1)))
         return false;

      dtQueryFilter f;

      // Iterate over all polys in our navmesh.
      const int MAX_SEGS = 6;
      for(U32 i = 0; i < nm->getMaxTiles(); ++i)
      {
         const dtMeshTile* tile = ((const dtNavMesh*)nm)->getTile(i);
         if(!tile->header) continue;
         const dtPolyRef base = nm->getPolyRefBase(tile);
         for(U32 j = 0; j < tile->header->polyCount; ++j)
         {
            const dtPolyRef ref = base | j;
            float segs[MAX_SEGS*6];
            int nsegs = 0;
            query->getPolyWallSegments(ref, &f, segs, NULL, &nsegs, MAX_SEGS);
            for(int j = 0; j < nsegs; ++j)
            {
               const float* sa = &segs[j*6];
               const float* sb = &segs[j*6+3];
               Point3F a = RCtoDTS(sa), b = RCtoDTS(sb);
               F32 len = (b - a).len();
               if(len < mWalkableRadius * 2)
                  continue;
               Point3F edge = b - a;
               edge.normalize();
               // Number of points to try placing - for now, one at each end.
               U32 pointCount = (len > mWalkableRadius * 4) ? 2 : 1;
               for(U32 i = 0; i < pointCount; i++)
               {
                  MatrixF mat;
                  Point3F pos;
                  // If we're only placing one point, put it in the middle.
                  if(pointCount == 1)
                     pos = a + edge * len / 2;
                  // Otherwise, stand off from edge ends.
                  else
                  {
                     if(i % 2)
                        pos = a + edge * (i/2+1) * mWalkableRadius;
                     else
                        pos = b - edge * (i/2+1) * mWalkableRadius;
                  }
                  CoverPointData data;
                  if(testEdgeCover(pos, edge, data))
                  {
                     CoverPoint *m = new CoverPoint();
                     if(!m->registerObject())
                        delete m;
                     else
                     {
                        m->setTransform(data.trans);
                        m->setSize(data.size);
                        m->setPeek(data.peek[0], data.peek[1], data.peek[2]);
                        if(set)
                           set->addObject(m);
                     }
                  }
               }
            }
         }
      }
      return true;
   }

   bool NavMesh::testEdgeCover(const Point3F &pos, const VectorF &dir, CoverPointData &data)
   {
      data.peek[0] = data.peek[1] = data.peek[2] = false;
      // Get the edge normal.
      Point3F norm;
      mCross(dir, Point3F(0, 0, 1), &norm);
      RayInfo ray;
      U32 hits = 0;
      for(U32 j = 0; j < CoverPoint::NumSizes; j++)
      {
         Point3F test = pos + Point3F(0.0f, 0.0f, mWalkableHeight * j / CoverPoint::NumSizes);
         if(getContainer()->castRay(test, test + norm * mCoverDist, StaticObjectType, &ray))
         {
            // Test peeking.
            Point3F left = test + dir * mPeekDist;
            data.peek[0] = !getContainer()->castRay(test, left, StaticObjectType, &ray)
               && !getContainer()->castRay(left, left + norm * mCoverDist, StaticObjectType, &ray);

            Point3F right = test - dir * mPeekDist;
            data.peek[1] = !getContainer()->castRay(test, right, StaticObjectType, &ray)
               && !getContainer()->castRay(right, right + norm * mCoverDist, StaticObjectType, &ray);

            Point3F over = test + Point3F(0, 0, 1) * 0.2f;
            data.peek[2] = !getContainer()->castRay(test, over, StaticObjectType, &ray)
               && !getContainer()->castRay(over, over + norm * mCoverDist, StaticObjectType, &ray);

            if(mInnerCover || data.peek[0] || data.peek[1] || data.peek[2])
               hits++;
            // If we couldn't peek here, we may be able to peek further up.
         }
         else
            // No cover at this height - break off.
            break;
      }
      if(hits > 0)
      {
         data.size = (CoverPoint::Size)(hits - 1);
         data.trans = MathUtils::createOrientFromDir(norm);
         data.trans.setPosition(pos);
      }
      return hits > 0;
   }

   void NavMesh::prepRenderImage(SceneRenderState *state)
   {
      ObjectRenderInst *ri = state->getRenderPass()->allocInst<ObjectRenderInst>();
      ri->renderDelegate.bind(this, &NavMesh::render);
      ri->type = RenderPassManager::RIT_Editor;      
      ri->translucentSort = true;
      ri->defaultKey = 1;
      state->getRenderPass()->addInst(ri);
   }

   void NavMesh::render(ObjectRenderInst *ri, SceneRenderState *state, BaseMatInstance *overrideMat)
   {
      if(overrideMat)
         return;

      if(state->isReflectPass())
         return;

      PROFILE_SCOPE(NavMesh_Render);

      // Recast debug draw
      duDebugDrawTorque dd;
      NetObject *no = getServerObject();
      if(no)
      {
         NavMesh *n = static_cast<NavMesh*>(no);

         if(n->isSelected())
         {
            GFXDrawUtil *drawer = GFX->getDrawUtil();

            GFXStateBlockDesc desc;
            desc.setZReadWrite(true, false);
            desc.setBlend(true);
            desc.setCullMode(GFXCullNone);

            drawer->drawCube(desc, getWorldBox(), ColorI(136, 228, 255, 45));
            desc.setFillModeWireframe();
            drawer->drawCube(desc, getWorldBox(), ColorI::BLACK);
         }

         bool build = n->mBuilding;
         if(build)
            dd.overrideColour(duRGBA(255, 0, 0, 80));
         if(!n->isSelected() || !Con::getBoolVariable("$Nav::EditorOpen"))
            dd.overrideAlpha(20);
         if(n->nm)
         {
            if(Con::getBoolVariable("$Nav::Editor::renderPortals")) duDebugDrawNavMeshPortals(&dd, *n->nm);
            if(Con::getBoolVariable("$Nav::Editor::renderBVTree"))  duDebugDrawNavMeshBVTree (&dd, *n->nm);
            if(Con::getBoolVariable("$Nav::Editor::renderMesh", 1)) duDebugDrawNavMesh       (&dd, *n->nm, 0);
         }
      }
   }

   void NavMesh::renderLinks(duDebugDraw &dd)
   {
      if(mBuilding)
         return;
      dd.depthMask(false);
      dd.begin(DU_DRAW_LINES);
      for(U32 i = 0; i < mLinkIDs.size(); i++)
      {
         unsigned int col = 0;
         switch(mLinkSelectStates[i])
         {
         case Unselected: col = mLinksUnsynced[i] ? duRGBA(255, 0, 0, 200) : duRGBA(0, 0, 255, 255); break;
         case Hovered: col = duRGBA(255, 255, 255, 255); break;
         case Selected: col = duRGBA(0, 255, 0, 255); break;
         }
         F32 *s = &mLinkVerts[i*6];
         F32 *e = &mLinkVerts[i*6 + 3];
         if(!mDeleteLinks[i])
            duAppendCircle(&dd, s[0], s[1], s[2], mLinkRads[i], col);
         duAppendArc(&dd,
            s[0], s[1], s[2],
            e[0], e[1], e[2],
            0.3f,
            0.0f, mLinkFlags[i] == DropFlag ? 0.0f : 0.4f,
            col);
         if(!mDeleteLinks[i])
            duAppendCircle(&dd, e[0], e[1], e[2], mLinkRads[i], col);
      }
      dd.end();
      dd.depthMask(true);
   }

   void NavMesh::renderTileData(duDebugDraw &dd, U32 tile)
   {
      if(tile >= mTileData.size())
         return;
      if(mTileData[tile].hf && Con::getBoolVariable("$Nav::Editor::renderVoxels", false))
         duDebugDrawHeightfieldWalkable(&dd, *mTileData[tile].hf);
      if(Con::getBoolVariable("$Nav::Editor::renderInput", false))
         mTileData[tile].geom.renderWire();
   }

   void NavMesh::onEditorEnable()
   {
      mNetFlags.set(Ghostable);
   }

   void NavMesh::onEditorDisable()
   {
      mNetFlags.clear(Ghostable);
   }

   U32 NavMesh::packUpdate(NetConnection *conn, U32 mask, BitStream *stream)
   {
      U32 retMask = Parent::packUpdate(conn, mask, stream);

      mathWrite(*stream, getTransform());
      mathWrite(*stream, getScale());

      return retMask;
   }

   void NavMesh::unpackUpdate(NetConnection *conn, BitStream *stream)
   {
      Parent::unpackUpdate(conn, stream);

      mathRead(*stream, &mObjToWorld);
      mathRead(*stream, &mObjScale);

      setTransform(mObjToWorld);
   }

   static const int NAVMESHSET_MAGIC = 'M'<<24 | 'S'<<16 | 'E'<<8 | 'T'; //'MSET';
   static const int NAVMESHSET_VERSION = 1;

   struct NavMeshSetHeader
   {
      int magic;
      int version;
      int numTiles;
      dtNavMeshParams params;
   };

   struct NavMeshTileHeader
   {
      dtTileRef tileRef;
      int dataSize;
   };

   bool NavMesh::load()
   {
      if(!dStrlen(mFileName))
         return false;

      FILE* fp = fopen(mFileName, "rb");
      if(!fp)
         return false;

      // Read header.
      NavMeshSetHeader header;
      fread(&header, sizeof(NavMeshSetHeader), 1, fp);
      if(header.magic != NAVMESHSET_MAGIC)
      {
         fclose(fp);
         return 0;
      }
      if(header.version != NAVMESHSET_VERSION)
      {
         fclose(fp);
         return 0;
      }

      if(nm)
         dtFreeNavMesh(nm);
      nm = dtAllocNavMesh();
      if(!nm)
      {
         fclose(fp);
         return false;
      }

      dtStatus status = nm->init(&header.params);
      if(dtStatusFailed(status))
      {
         fclose(fp);
         return false;
      }

      // Read tiles.
      for(U32 i = 0; i < header.numTiles; ++i)
      {
         NavMeshTileHeader tileHeader;
         fread(&tileHeader, sizeof(tileHeader), 1, fp);
         if(!tileHeader.tileRef || !tileHeader.dataSize)
            break;

         unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
         if(!data) break;
         memset(data, 0, tileHeader.dataSize);
         fread(data, tileHeader.dataSize, 1, fp);

         nm->addTile(data, tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, 0);
      }

      S32 s;
      fread(&s, sizeof(S32), 1, fp);
      setLinkCount(s);
      fread(const_cast<F32*>(mLinkVerts.address()), sizeof(F32), s * 6, fp);
      fread(const_cast<F32*>(mLinkRads.address()), sizeof(F32), s, fp);
      fread(const_cast<U8*>(mLinkDirs.address()), sizeof(U8), s, fp);
      fread(const_cast<U8*>(mLinkAreas.address()), sizeof(U8), s, fp);
      fread(const_cast<unsigned short*>(mLinkFlags.address()), sizeof(unsigned short), s, fp);
      fread(const_cast<U32*>(mLinkIDs.address()), sizeof(U32), s, fp);
      mLinksUnsynced.fill(false);
      mLinkSelectStates.fill(Unselected);
      mDeleteLinks.fill(false);

      fclose(fp);

      updateTiles();

      if(isServerObject())
      {
         setMaskBits(LoadFlag);
         if(getEventManager())
            getEventManager()->postEvent("NavMeshUpdate", getIdString());
      }

      return true;
   }

   bool NavMesh::save()
   {
#ifdef WALKABOUT_DEMO
      Con::executef("OnWalkaboutDemoSave");
      return false;
#else
      if(!dStrlen(mFileName) || !nm)
         return false;

      // Save our navmesh into a file to load from next time
      FILE* fp = fopen(mFileName, "wb");
      if(!fp)
         return false;

      // Store header.
      NavMeshSetHeader header;
      header.magic = NAVMESHSET_MAGIC;
      header.version = NAVMESHSET_VERSION;
      header.numTiles = 0;
      for(U32 i = 0; i < nm->getMaxTiles(); ++i)
      {
         const dtMeshTile* tile = ((const dtNavMesh*)nm)->getTile(i);
         if (!tile || !tile->header || !tile->dataSize) continue;
         header.numTiles++;
      }
      memcpy(&header.params, nm->getParams(), sizeof(dtNavMeshParams));
      fwrite(&header, sizeof(NavMeshSetHeader), 1, fp);

      // Store tiles.
      for(U32 i = 0; i < nm->getMaxTiles(); ++i)
      {
         const dtMeshTile* tile = ((const dtNavMesh*)nm)->getTile(i);
         if(!tile || !tile->header || !tile->dataSize) continue;

         NavMeshTileHeader tileHeader;
         tileHeader.tileRef = nm->getTileRef(tile);
         tileHeader.dataSize = tile->dataSize;
         fwrite(&tileHeader, sizeof(tileHeader), 1, fp);

         fwrite(tile->data, tile->dataSize, 1, fp);
      }

      S32 s = mLinkIDs.size();
      fwrite(&s, sizeof(S32), 1, fp);
      fwrite(mLinkVerts.address(), sizeof(F32), s * 6, fp);
      fwrite(mLinkRads.address(), sizeof(F32), s, fp);
      fwrite(mLinkDirs.address(), sizeof(U8), s, fp);
      fwrite(mLinkAreas.address(), sizeof(U8), s, fp);
      fwrite(mLinkFlags.address(), sizeof(unsigned short), s, fp);
      fwrite(mLinkIDs.address(), sizeof(U32), s, fp);

      fclose(fp);

      return true;
#endif
   }

   void NavMesh::write(Stream &stream, U32 tabStop, U32 flags)
   {
      save();
      Parent::write(stream, tabStop, flags);
   }

   DefineEngineMethod(NavMesh, build, bool, (),,
      "@brief Create a Recast nav mesh.")
   {
      return object->build();
   }

   DefineEngineMethod(NavMesh, createCoverPoints, bool, (),,
      "@brief Create cover points for this NavMesh.")
   {
      return object->createCoverPoints();
   }

   DefineEngineMethod(NavMesh, deleteCoverPoints, void, (),,
      "@brief Remove all cover points for this NavMesh.")
   {
      object->deleteCoverPoints();
   }

   DefineEngineMethod(NavMesh, buildTiles, void, (Box3F box),,
      "@brief Rebuild the tiles overlapped by the input box.")
   {
      return object->buildTiles(box);
   }

   DefineEngineMethod(NavMesh, buildLinks, void, (),,
      "@brief Build tiles of this mesh where there are unsynchronised links.")
   {
      object->buildLinks();
   }

   DefineEngineMethod(NavMesh, load, bool, (),,
      "@brief Load this NavMesh from its file.")
   {
      return object->load();
   }

   DefineEngineMethod(NavMesh, save, void, (),,
      "@brief Save this NavMesh to its file.")
   {
      object->save();
   }

   DefineConsoleFunction(getNavMeshEventManager, S32, (),,
      "@brief Get the EventManager object for all NavMesh updates.")
   {
      return NavMesh::getEventManager()->getId();
   }

   DefineConsoleFunction(WalkaboutUpdateAll, void, (S32 objid, bool remove), (0, false),
      "@brief Update all NavMesh tiles that intersect the given object's world box.")
   {
      SceneObject *obj;
      if(!Sim::findObject(objid, obj))
         return;
      if(remove)
         obj->disableCollision();
      SimSet *set = NavMesh::getServerSet();
      for(U32 i = 0; i < set->size(); i++)
      {
         NavMesh *m = static_cast<Nav::NavMesh*>(set->at(i));
         m->buildTiles(obj->getWorldBox());
      }
      if(remove)
         obj->enableCollision();
   }

   DefineConsoleFunction(WalkaboutUpdateMesh, void, (S32 meshid, S32 objid, bool remove), (0, 0, false),
      "@brief Update all tiles in a given NavMesh that intersect the given object's world box.")
   {
      NavMesh *mesh;
      SceneObject *obj;
      if(!Sim::findObject(meshid, mesh))
      {
         Con::errorf("WalkaboutUpdateMesh: cannot find NavMesh %d", meshid);
         return;
      }
      if(!Sim::findObject(objid, obj))
      {
         Con::errorf("WalkaboutUpdateMesh: cannot find SceneObject %d", objid);
         return;
      }
      if(remove)
         obj->disableCollision();
      mesh->buildTiles(obj->getWorldBox());
      if(remove)
         obj->enableCollision();
   }
};
