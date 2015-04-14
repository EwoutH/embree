// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh4i/bvh4i.h"
#include "bvh4i/bvh4i_builder.h"
#include "bvh4i/bvh4i_rotate.h"
#include "common/subdiv/feature_adaptive_gregory.h"

#define PRESPLIT_SPACE_FACTOR         1.30f

// #define PRESPLIT_AREA_THRESHOLD      20.0f
// #define PRESPLIT_MIN_AREA             0.01f

#define NUM_PRESPLITS_PER_TRIANGLE    16
#define PRESPLITS_TREE_DEPTH          4

#define DBG(x) 
#define TIMER(x) 

#define L1_PREFETCH_ITEMS 2
#define L2_PREFETCH_ITEMS 16

//FIXME: use 8-bytes compact prim ref is used

namespace embree
{

  /* =================================================================================== */
  /* =================================================================================== */
  /* =================================================================================== */

  struct __aligned(16) PreSplitID
  {
    unsigned int code;
    float sah;
    unsigned int groupID;
    unsigned int primID;
            
    __forceinline operator unsigned int() const { return code; }

    __forceinline unsigned int get(const unsigned int shift, const unsigned and_mask) const {
      return (code >> shift) & and_mask;
    }

    __forceinline unsigned int getByte(const size_t b) const {
      assert(b < 4);
      const unsigned char *__restrict const ptr = (const unsigned char*)&code;
      return ptr[b];
    }
            
    __forceinline friend std::ostream &operator<<(std::ostream &o, const PreSplitID& pid) {
      o << "code " << pid.code << " boxSAH = " << pid.sah << " groupID " << pid.groupID << " primID " << pid.primID;
      return o;
    }
    __forceinline bool operator<(const PreSplitID &pid) const { return code < pid.code; } 
    __forceinline bool operator>(const PreSplitID &pid) const { return code > pid.code; } 
  };

  void BVH4iBuilderPreSplits::printBuilderName()
  {
    std::cout << "building BVH4i with presplits-based SAH builder (MIC) ... " << std::endl;    
  }

  void BVH4iBuilderPreSplits::allocateData(const size_t threadCount, const size_t totalNumPrimitives)
  {
    size_t numPrimitivesOld = numPrimitives;
    numPrimitives = totalNumPrimitives;

    if (numPrimitivesOld != numPrimitives)
      {
	const size_t numPrims = (size_t)((float)numPrimitives * PRESPLIT_SPACE_FACTOR);
PRINT(numPrims);
	const size_t CORRECT_numPrims = (size_t)((size_t)numPrimitives * PRESPLIT_SPACE_FACTOR);
PRINT(CORRECT_numPrims);
	const size_t minAllocNodes = (threadCount+1) * ALLOCATOR_NODE_BLOCK_SIZE;
	const size_t numNodes = (size_t)((numPrims+3)/4) + minAllocNodes;

	numMaxPrimitives = numPrims;
	numMaxPreSplits  = numPrims - numPrimitives;

	if (State::instance()->verbosity(2))
	  {
	    PRINT(numPrimitives);
	    PRINT(numMaxPrimitives);
	    PRINT(numMaxPreSplits);
	  };

	allocateMemoryPools(numPrims,numNodes);
      }    
  }


  __forceinline void splitTri(const PrimRef& prim, int dim, float pos, const Vec3fa& a, const Vec3fa& b, const Vec3fa& c, PrimRef& left_o, PrimRef& right_o)
  {
    BBox3fa left = empty, right = empty;
    const Vec3fa v[3] = { a,b,c };

    /* clip triangle to left and right box by processing all edges */
    Vec3fa v1 = v[2];
    for (size_t i=0; i<3; i++)
      {
	Vec3fa v0 = v1; v1 = v[i];
	float v0d = v0[dim], v1d = v1[dim];
      
	if (v0d <= pos) left. extend(v0); // this point is on left side
	if (v0d >= pos) right.extend(v0); // this point is on right side

	if ((v0d < pos && pos < v1d) || (v1d < pos && pos < v0d)) // the edge crosses the splitting location
	  {
	    assert((v1d-v0d) != 0.0f);
	    Vec3fa c = v0 + (pos-v0d)/(v1d-v0d)*(v1-v0);
	    left.extend(c);
	    right.extend(c);
	  }
      }
    assert(!left.empty());  // happens if split does not hit triangle
    assert(!right.empty()); // happens if split does not hit triangle

    /* safe clip against current bounds */
    BBox3fa bounds = prim.bounds();
    BBox3fa cleft(min(max(left.lower,bounds.lower),bounds.upper),
                 max(min(left.upper,bounds.upper),bounds.lower));
    BBox3fa cright(min(max(right.lower,bounds.lower),bounds.upper),
                  max(min(right.upper,bounds.upper),bounds.lower));

    left_o  = PrimRef(cleft, prim.geomID(), prim.primID());
    right_o = PrimRef(cright,prim.geomID(), prim.primID());
  }

  __forceinline mic_f box_sah( const mic_f &b_min,
			       const mic_f &b_max) 
  { 
    const mic_f d = b_max - b_min;
    const mic_f d_x = swAAAA(d);
    const mic_f d_y = swBBBB(d);
    const mic_f d_z = swCCCC(d);
    return (d_x*(d_y+d_z)+d_y*d_z)*2.0f; 
  }

  __forceinline mic_f box_sah( const PrimRef &r) 
  { 
    const mic_f bmin = broadcast4to16f(&r.lower);
    const mic_f bmax = broadcast4to16f(&r.upper);
    return box_sah(bmin,bmax);
  }

  __forceinline mic_f tri_sah( const mic_f &v0,
			       const mic_f &v1,
			       const mic_f &v2) 
  {
    const mic_f n = lcross_xyz(v1-v0,v2-v0);
    return sqrt(ldot3_xyz(n,n)) * 0.5f;
  }
  
  __forceinline size_t getMaxDim(const PrimRef &p)
  {
    Vec3fa diag = p.upper - p.lower;
    size_t index = 0;
    for (size_t i=1;i<3;i++)
      if (diag[i] > diag[index])
	index = i;
    return index;
  }

  void subdivideTriangle(const PrimRef &primBounds,
			 const Vec3fa& vtxA,
			 const Vec3fa& vtxB,
			 const Vec3fa& vtxC,
			 const size_t depth,
			 AlignedAtomicCounter32 &counter,
			 PrimRef *__restrict__ prims)
  {
    if (depth == 0) 
      {	    
	const unsigned int index = counter.inc();
	prims[index] = primBounds;
      }
    else
      {
	const size_t dim = getMaxDim(primBounds);
	
	PrimRef left,right;

	const float pos = (primBounds.upper[dim] + primBounds.lower[dim]) * 0.5f;
	splitTri(primBounds,dim,pos,vtxA,vtxB,vtxC,left,right);

	subdivideTriangle( left ,vtxA,vtxB,vtxC, depth-1,counter,prims);
	subdivideTriangle( right,vtxA,vtxB,vtxC, depth-1,counter,prims);
      }
  }


  void BVH4iBuilderPreSplits::computePrimRefs(const size_t threadIndex, const size_t threadCount)
  {
    TIMER(double msec = 0.0);
    TIMER(msec = getSeconds());

    // =============================================================================================================

    averageBoxSAH = 0.0f;
    averageTriSAH = 0.0f;

    scene->lockstep_scheduler.dispatchTask( task_getAverageBoundsSAH, this, threadIndex, threadCount );

    averageBoxSAH /= (float)numPrimitives;
    averageTriSAH /= (float)numPrimitives;

    averageBoxTriSAHRatio = averageBoxSAH / averageTriSAH;

    PRINT(averageBoxSAH);
    PRINT(averageTriSAH);
    PRINT(averageBoxTriSAHRatio);
    
#if 1
    PRESPLIT_AREA_THRESHOLD = 20.0f;
    PRESPLIT_MIN_AREA       = 0.01f;
#else
    PRESPLIT_AREA_THRESHOLD = 10.0f *averageBoxTriSAHRatio;
    PRESPLIT_MIN_AREA       = 1.6f * averageBoxSAH;

#endif
    // =============================================================================================================

    dest0.reset(0);
    dest1.reset(0);

    scene->lockstep_scheduler.dispatchTask( task_countAndComputePrimRefsPreSplits, this, threadIndex, threadCount );

    const unsigned int preSplits        = dest1;

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_countAndComputePrimRefsPreSplits " << 1000. * msec << " ms" << std::endl << std::flush);

    TIMER(msec = getSeconds());

    radix_sort_u32((PreSplitID*)accel,((PreSplitID*)accel) + preSplits,preSplits);

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_radixSortPreSplitIDs " << 1000. * msec << " ms" << std::endl << std::flush);


    TIMER(msec = getSeconds());

    scene->lockstep_scheduler.dispatchTask( task_computePrimRefsFromPreSplitIDs, this, threadIndex, threadCount );

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_computePrimRefsFromPreSplitIDs " << 1000. * msec << " ms" << std::endl << std::flush);


    numPrimitives = dest0;

  }


  void BVH4iBuilderPreSplits::getAverageBoundsSAH(const size_t threadID, const size_t numThreads) 
  {
    const size_t numGroups = scene->size();
    const size_t startID = (threadID+0)*numPrimitives/numThreads;
    const size_t endID   = (threadID+1)*numPrimitives/numThreads;
    const size_t items   = endID - startID;

    // === find first group containing startID ===
    unsigned int startGroup=0, numSkipped = 0;
    for (; startGroup<numGroups; startGroup++) {       
      if (unlikely(scene->get(startGroup) == nullptr)) continue;
      if (unlikely(scene->get(startGroup)->type != Geometry::TRIANGLE_MESH)) continue;
      const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(startGroup);
      if (unlikely(!mesh->isEnabled())) continue;
      if (unlikely(mesh->numTimeSteps != 1)) continue;

      const size_t numTriangles = mesh->size();
      if (numSkipped + numTriangles > startID) break;
      numSkipped += numTriangles;
    }

    float sahBox = 0.0f;
    float sahTri = 0.0f;

    // === determine presplit candidates ===
    {
      unsigned int currentID = startID;
      unsigned int offset = startID - numSkipped;
      for (unsigned int g=startGroup; g<numGroups; g++) 
	{
	  if (unlikely(scene->get(g) == nullptr)) continue;
	  if (unlikely(scene->get(g)->type != Geometry::TRIANGLE_MESH)) continue;
	  const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(g);
	  if (unlikely(!mesh->isEnabled())) continue;
	  if (unlikely(mesh->numTimeSteps != 1)) continue;

	  if (offset < mesh->size())
	    {
	      const char* __restrict__ cptr_tri = (char*)&mesh->triangle(offset);
	      const unsigned int stride = mesh->triangles.getBufferStride();

	      for (unsigned int i=offset; i<mesh->size() && currentID < endID; i++, currentID++,cptr_tri+=stride)	 
		{ 			    
		  const TriangleMesh::Triangle& tri = *(TriangleMesh::Triangle*)cptr_tri;
		  prefetch<PFHINT_L2>(&tri + L2_PREFETCH_ITEMS);
		  prefetch<PFHINT_L1>(&tri + L1_PREFETCH_ITEMS);

		  const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);

		  mic_f bmin = min(min(v[0],v[1]),v[2]);
		  mic_f bmax = max(max(v[0],v[1]),v[2]);

		  const mic_f area_tri = tri_sah(v[0],v[1],v[2]);
		  const mic_f area_box = box_sah(bmin,bmax);
		  const mic_f factor = area_box * rcp(area_tri);
		  
		  sahBox += area_box[0];
		  sahTri += area_tri[0];

		}
	    }
	  if (currentID == endID) break;
	  offset = 0;
	}
    }

    atomic_add_f32(&averageBoxSAH,sahBox);    
    atomic_add_f32(&averageTriSAH,sahTri);    
  }

  void BVH4iBuilderPreSplits::countAndComputePrimRefsPreSplits(const size_t threadID, const size_t numThreads) 
  {
    const size_t numGroups = scene->size();
    const size_t startID = (threadID+0)*numPrimitives/numThreads;
    const size_t endID   = (threadID+1)*numPrimitives/numThreads;
    

    // === find first group containing startID ===
    unsigned int startGroup=0, numSkipped = 0;
    for (; startGroup<numGroups; startGroup++) {       
      if (unlikely(scene->get(startGroup) == nullptr)) continue;
      if (unlikely(scene->get(startGroup)->type != Geometry::TRIANGLE_MESH)) continue;
      const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(startGroup);
      if (unlikely(!mesh->isEnabled())) continue;
      if (unlikely(mesh->numTimeSteps != 1)) continue;

      const size_t numTriangles = mesh->size();
      if (numSkipped + numTriangles > startID) break;
      numSkipped += numTriangles;
    }

    // === start with first group containing startID ===
    mic_f bounds_scene_min((float)pos_inf);
    mic_f bounds_scene_max((float)neg_inf);
    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);

    unsigned int numTrisNoPreSplit = 0;
    unsigned int numTrisPreSplit = 0;

    // === determine presplit candidates ===
    {
      unsigned int currentID = startID;
      unsigned int offset = startID - numSkipped;
      for (unsigned int g=startGroup; g<numGroups; g++) 
	{
	  if (unlikely(scene->get(g) == nullptr)) continue;
	  if (unlikely(scene->get(g)->type != Geometry::TRIANGLE_MESH)) continue;
	  const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(g);
	  if (unlikely(!mesh->isEnabled())) continue;
	  if (unlikely(mesh->numTimeSteps != 1)) continue;

	  if (offset < mesh->size())
	    {
	      const char* __restrict__ cptr_tri = (char*)&mesh->triangle(offset);
	      const unsigned int stride = mesh->triangles.getBufferStride();

	      for (unsigned int i=offset; i<mesh->size() && currentID < endID; i++, currentID++,cptr_tri+=stride)	 
		{ 			    
		  const TriangleMesh::Triangle& tri = *(TriangleMesh::Triangle*)cptr_tri;
		  //const TriangleMesh::Triangle& tri = mesh->triangle(i);
		  prefetch<PFHINT_L2>(&tri + L2_PREFETCH_ITEMS);
		  prefetch<PFHINT_L1>(&tri + L1_PREFETCH_ITEMS);

		  const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);

		  mic_f bmin = min(min(v[0],v[1]),v[2]);
		  mic_f bmax = max(max(v[0],v[1]),v[2]);

		  const mic_f area_tri = tri_sah(v[0],v[1],v[2]);
		  const mic_f area_box = box_sah(bmin,bmax);
		  const mic_f factor = area_box * rcp(area_tri);

		  DBG(
		      PRINT(area_tri);
		      PRINT(area_box);
		      PRINT(factor);
		      );

		  const mic_m m_factor = factor > PRESPLIT_AREA_THRESHOLD;
		  const mic_m m_sah_zero = area_box > PRESPLIT_MIN_AREA;

		  if (any(m_factor & m_sah_zero)) 
		    numTrisPreSplit++;
		  else
		    numTrisNoPreSplit++;   

		  bounds_scene_min = min(bounds_scene_min,bmin);
		  bounds_scene_max = max(bounds_scene_max,bmax);
		  const mic_f centroid2 = bmin+bmax;
		  bounds_centroid_min = min(bounds_centroid_min,centroid2);
		  bounds_centroid_max = max(bounds_centroid_max,centroid2);
		}
	    }
	  if (currentID == endID) break;
	  offset = 0;
	}
    }

    /* update global bounds */
    Centroid_Scene_AABB bounds;
    
    store4f(&bounds.centroid2.lower,bounds_centroid_min);
    store4f(&bounds.centroid2.upper,bounds_centroid_max);
    store4f(&bounds.geometry.lower,bounds_scene_min);
    store4f(&bounds.geometry.upper,bounds_scene_max);

    global_bounds.extend_atomic(bounds);    


    const unsigned int no_presplit_index = dest0.add(numTrisNoPreSplit);
    const unsigned int presplit_index    = dest1.add(numTrisPreSplit);

    PrimRef *presplits = (PrimRef*)accel;

    PrimRef    *__restrict__ no_presplit_prims = this->prims + no_presplit_index;
    PreSplitID *__restrict__ presplitIDs       = (PreSplitID*)accel + presplit_index;


    // === put triangles into prepsplit/no-presplit bucket ===
    {
      unsigned int currentID = startID;
      unsigned int offset = startID - numSkipped;
      for (unsigned int g=startGroup; g<numGroups; g++) 
	{
	  if (unlikely(scene->get(g) == nullptr)) continue;
	  if (unlikely(scene->get(g)->type != Geometry::TRIANGLE_MESH)) continue;
	  const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(g);
	  if (unlikely(!mesh->isEnabled())) continue;
	  if (unlikely(mesh->numTimeSteps != 1)) continue;

	  if (offset < mesh->size())
	    {
	      const char* __restrict__ cptr_tri = (char*)&mesh->triangle(offset);
	      const unsigned int stride = mesh->triangles.getBufferStride();

	      for (unsigned int i=offset; i<mesh->size() && currentID < endID; i++, currentID++,cptr_tri+=stride)	 
		{ 			    
		  //const TriangleMesh::Triangle& tri = mesh->triangle(i);
		  const TriangleMesh::Triangle& tri = *(TriangleMesh::Triangle*)cptr_tri;

		  prefetch<PFHINT_L2>(&tri + L2_PREFETCH_ITEMS);
		  prefetch<PFHINT_L1>(&tri + L1_PREFETCH_ITEMS);

		  const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);

		  mic_f bmin = min(min(v[0],v[1]),v[2]);
		  mic_f bmax = max(max(v[0],v[1]),v[2]);

		  const mic_f area_tri = tri_sah(v[0],v[1],v[2]);
		  const mic_f area_box = box_sah(bmin,bmax);
		  const mic_f factor = area_box * rcp(area_tri);

		  DBG(
		      PRINT(area_tri);
		      PRINT(area_box);
		      PRINT(factor);
		      );

		  const mic_m m_factor = factor > PRESPLIT_AREA_THRESHOLD;
		  const mic_m m_sah_zero = area_box > PRESPLIT_MIN_AREA;

		  if (any(m_factor & m_sah_zero)) 
		    {
		      prefetch<PFHINT_L2EX>(presplitIDs + 4*4);

		      store1f(&presplitIDs->code,area_box);
		      store1f(&presplitIDs->sah,factor);
		      presplitIDs->groupID = g;
		      presplitIDs->primID  = i;
		      presplitIDs++;
		    }
		  else
		    {
		      prefetch<PFHINT_L2EX>(no_presplit_prims + L2_PREFETCH_ITEMS);

		      store4f(&no_presplit_prims->lower,bmin);
		      store4f(&no_presplit_prims->upper,bmax);	
		      no_presplit_prims->lower.a = g; 
		      no_presplit_prims->upper.a = i;
		      no_presplit_prims++;

		    }

		}
	    }
	  if (currentID == endID) break;
	  offset = 0;
	}
    }
  }


  void BVH4iBuilderPreSplits::computePrimRefsFromPreSplitIDs(const size_t threadID, const size_t numThreads) 
  {
    const size_t preSplitIDs    = dest1;
    const size_t step           = (numMaxPreSplits+NUM_PRESPLITS_PER_TRIANGLE-1) / NUM_PRESPLITS_PER_TRIANGLE;
    const size_t startPreSplits = (step <= preSplitIDs) ? preSplitIDs - step : preSplitIDs;

    // === no pre-splits ==
    {
      const size_t startID = (threadID+0)*startPreSplits/numThreads;
      const size_t endID   = (threadID+1)*startPreSplits/numThreads;
      const size_t items   = endID - startID;
      const unsigned int d_index = dest0.add(items);

      PrimRef    *__restrict__ prims = this->prims + d_index;
      PreSplitID *__restrict__ presplitIDs = (PreSplitID*)accel;
      
      for (size_t i=startID;i<endID;i++)
	{
	  prefetch<PFHINT_L2>(&presplitIDs[i] + L2_PREFETCH_ITEMS);
	  prefetch<PFHINT_L1>(&presplitIDs[i] + L1_PREFETCH_ITEMS);

	  const size_t geomID = presplitIDs[i].groupID;
	  const size_t primID = presplitIDs[i].primID;

	  const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	  const TriangleMesh::Triangle & tri = mesh->triangle(primID);

	  const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);

	  prefetch<PFHINT_L1EX>(prims);

	  mic_f bmin = min(min(v[0],v[1]),v[2]);
	  mic_f bmax = max(max(v[0],v[1]),v[2]);

	  prefetch<PFHINT_L2EX>(prims + L2_PREFETCH_ITEMS);

	  store4f(&prims->lower,bmin);
	  store4f(&prims->upper,bmax);	
	  prims->lower.a = geomID; 
	  prims->upper.a = primID;
	  prims++;
	}
    }

    // === performing pre-splitting ==
    {
      AlignedAtomicCounter32 counter;
      counter.reset(0);

      const unsigned int numPreSplits = preSplitIDs - startPreSplits;
      const size_t startID = (threadID+0)*numPreSplits/numThreads;
      const size_t endID   = (threadID+1)*numPreSplits/numThreads;
      const size_t items   = (endID - startID)*NUM_PRESPLITS_PER_TRIANGLE;
      const unsigned int d_index = dest0.add(items);

      PrimRef    *__restrict__ prims = this->prims + d_index;
      PreSplitID *__restrict__ presplitIDs = (PreSplitID*)accel + startPreSplits;
      
      for (size_t i=startID;i<endID;i++)
	{
	  prefetch<PFHINT_L2>(&presplitIDs[i] + L2_PREFETCH_ITEMS);
	  prefetch<PFHINT_L1>(&presplitIDs[i] + L1_PREFETCH_ITEMS);

	  const size_t geomID = presplitIDs[i].groupID;
	  const size_t primID = presplitIDs[i].primID;

	  const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	  const TriangleMesh::Triangle & tri = mesh->triangle(primID);

	  const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);

	  
	  mic_f bmin = min(min(v[0],v[1]),v[2]);
	  mic_f bmax = max(max(v[0],v[1]),v[2]);

	  const mic_f area_tri = tri_sah(v[0],v[1],v[2]);
	  const mic_f area_box = box_sah(bmin,bmax);

	  // FIXME: use store4f
	  Vec3fa vtxA = *(Vec3fa*)&v[0];
	  Vec3fa vtxB = *(Vec3fa*)&v[1];
	  Vec3fa vtxC = *(Vec3fa*)&v[2];

	  BBox3fa bounds = empty;
	  bounds.extend(vtxA);
	  bounds.extend(vtxB);
	  bounds.extend(vtxC);
	  PrimRef primBounds = PrimRef(bounds,geomID,primID);

	  subdivideTriangle(primBounds,vtxA,vtxB,vtxC,PRESPLITS_TREE_DEPTH,counter,prims);
	}
      assert(counter == items);
    }

  }

  void BVH4iBuilderPreSplits::finalize(const size_t threadIndex, const size_t threadCount)
  {
  }

  /* =================================================================================== */
  /* =================================================================================== */
  /* =================================================================================== */

  void BVH4iBuilderVirtualGeometry::printBuilderName()
  {
    std::cout << "building BVH4i with Virtual Geometry SAH builder (MIC) ... " << std::endl;    
  }

  size_t BVH4iBuilderVirtualGeometry::getNumPrimitives()
  {
    /* count total number of virtual objects */
    size_t numVirtualObjects = 0;       
    for (size_t i=0;i<scene->size();i++)
      {
	if (unlikely(scene->get(i) == nullptr)) continue;
	if (unlikely((scene->get(i)->type != Geometry::USER_GEOMETRY) /*&& (scene->get(i)->type != INSTANCES)*/)) continue;
	if (unlikely(!scene->get(i)->isEnabled())) continue;
        UserGeometryBase* geom = (UserGeometryBase*) scene->get(i);
	numVirtualObjects += geom->size();
      }
    return numVirtualObjects;	
  }

  void BVH4iBuilderVirtualGeometry::computePrimRefs(const size_t threadIndex, const size_t threadCount)
  {
    scene->lockstep_scheduler.dispatchTask( task_computePrimRefsVirtualGeometry, this, threadIndex, threadCount );	
  }

  void BVH4iBuilderVirtualGeometry::createAccel(const size_t threadIndex, const size_t threadCount)
  {
    scene->lockstep_scheduler.dispatchTask( task_createVirtualGeometryAccel, this, threadIndex, threadCount );
  }

  void BVH4iBuilderVirtualGeometry::computePrimRefsVirtualGeometry(const size_t threadID, const size_t numThreads) 
  {
    const size_t numTotalGroups = scene->size();

    /* count total number of virtual objects */
    const size_t numVirtualObjects = numPrimitives;
    const size_t startID   = (threadID+0)*numVirtualObjects/numThreads;
    const size_t endID     = (threadID+1)*numVirtualObjects/numThreads; 
    
    PrimRef *__restrict__ const prims     = this->prims;

    // === find first group containing startID ===
    unsigned int g=0, numSkipped = 0;
    for (; g<numTotalGroups; g++) {       
      if (unlikely(scene->get(g) == nullptr)) continue;
      if (unlikely((scene->get(g)->type != Geometry::USER_GEOMETRY) /*&& (scene->get(g)->type != INSTANCES)*/)) continue;
      if (unlikely(!scene->get(g)->isEnabled())) continue;
      const UserGeometryBase* const geom = (UserGeometryBase*) scene->get(g);
      const size_t numPrims = geom->size();
      if (numSkipped + numPrims > startID) break;
      numSkipped += numPrims;
    }

    /* start with first group containing startID */
    mic_f bounds_scene_min((float)pos_inf);
    mic_f bounds_scene_max((float)neg_inf);
    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);

    unsigned int num = 0;
    unsigned int currentID = startID;
    unsigned int offset = startID - numSkipped;

    for (; g<numTotalGroups; g++) 
      {
	if (unlikely(scene->get(g) == nullptr)) continue;
	if (unlikely((scene->get(g)->type != Geometry::USER_GEOMETRY ) /*&& (scene->get(g)->type != INSTANCES)*/)) continue;
	if (unlikely(!scene->get(g)->isEnabled())) continue;

	UserGeometryBase *virtual_geometry = (UserGeometryBase *)scene->get(g);

        size_t N = virtual_geometry->size();
        for (unsigned int i=offset; i<N && currentID < endID; i++, currentID++)	 
	  { 			    
	    const BBox3fa bounds = virtual_geometry->bounds(i);
	    const mic_f bmin = broadcast4to16f(&bounds.lower); 
	    const mic_f bmax = broadcast4to16f(&bounds.upper);
          
	    bounds_scene_min = min(bounds_scene_min,bmin);
	    bounds_scene_max = max(bounds_scene_max,bmax);
	    const mic_f centroid2 = bmin+bmax;
	    bounds_centroid_min = min(bounds_centroid_min,centroid2);
	    bounds_centroid_max = max(bounds_centroid_max,centroid2);

	    store4f(&prims[currentID].lower,bmin);
	    store4f(&prims[currentID].upper,bmax);	
	    prims[currentID].lower.a = g;
	    prims[currentID].upper.a = i;
	  }
        if (currentID == endID) break;
        offset = 0;
      }

    /* update global bounds */
    Centroid_Scene_AABB bounds;
    
    store4f(&bounds.centroid2.lower,bounds_centroid_min);
    store4f(&bounds.centroid2.upper,bounds_centroid_max);
    store4f(&bounds.geometry.lower,bounds_scene_min);
    store4f(&bounds.geometry.upper,bounds_scene_max);

    global_bounds.extend_atomic(bounds);    
  }


  void BVH4iBuilderVirtualGeometry::createVirtualGeometryAccel(const size_t threadID, const size_t numThreads)
  {
    const size_t startID = (threadID+0)*numPrimitives/numThreads;
    const size_t endID   = (threadID+1)*numPrimitives/numThreads;

    AccelSetItem *acc = (AccelSetItem*)accel + startID;

    const PrimRef* __restrict__  bptr = prims + startID;

    for (size_t j=startID; j<endID; j++, bptr++, acc++)
      {
	prefetch<PFHINT_NT>(bptr + L1_PREFETCH_ITEMS);
	prefetch<PFHINT_L2>(bptr + L2_PREFETCH_ITEMS);
	assert(bptr->geomID() < scene->size() );
        AccelSet* _accel = (AccelSet*)(UserGeometryBase *) scene->get( bptr->geomID() );
	acc->accel = _accel;
        acc->item = bptr->primID();
      }
  }



  // ==========================================================================================
  // ==========================================================================================
  // ==========================================================================================


  void BVH4iBuilderMemoryConservative::printBuilderName()
  {
    std::cout << "building BVH4i with memory conservative binned SAH builder (MIC) ... " << std::endl;    
  }

  void BVH4iBuilderMemoryConservative::allocateData(const size_t threadCount, const size_t totalNumPrimitives)
  {
    enableTaskStealing = true;
    enablePerCoreWorkQueueFill = true; 
    
    size_t numPrimitivesOld = numPrimitives;
    numPrimitives = totalNumPrimitives;

    if (numPrimitivesOld != numPrimitives)
      {
	const size_t numPrims = numPrimitives;
	const size_t minAllocNodes = (threadCount+1) * 2 * ALLOCATOR_NODE_BLOCK_SIZE;
	const size_t numNodes = max((size_t)((numPrims+3)/4),minAllocNodes);
	const size_t sizeNodeInBytes   = sizeof(BVH4i::QuantizedNode);
	const size_t sizeAccelInBytes  = sizeof(Triangle1mc);

	allocateMemoryPools(numPrims,numNodes,sizeNodeInBytes,sizeAccelInBytes);	
      }    
  }

  void BVH4iBuilderMemoryConservative::finalize(const size_t threadIndex, const size_t threadCount)
  {

  }

  void BVH4iBuilderMemoryConservative::storeNodeDataUpdateParentPtrs(void *ptr,
								     BuildRecord *__restrict__ const br,
								     const size_t numChildren)
  {
    BVH4i::QuantizedNode *__restrict__ cnode = (BVH4i::QuantizedNode*)ptr;
    for (size_t i=0;i<numChildren;i++)
      br[i].parentPtr = &cnode->child(i);

    BVH4i::Node tmp;
    storeNode(&tmp,br,numChildren);    
    cnode->init(tmp);
  }


  void BVH4iBuilderMemoryConservative::createAccel(const size_t threadIndex, const size_t threadCount)
  {
    scene->lockstep_scheduler.dispatchTask( task_createMemoryConservativeAccel, this, threadIndex, threadCount );  

    // === do some padding add the end of 'accel' ===

    prims[numPrimitives+0] = prims[numPrimitives-1];
    prims[numPrimitives+1] = prims[numPrimitives-1];
    prims[numPrimitives+2] = prims[numPrimitives-1];

    // === 'prims' became 'accel' === 
    prims = nullptr;
    size_prims = 0;

  }


  void BVH4iBuilderMemoryConservative::createMemoryConservativeAccel(const size_t threadID, const size_t numThreads)
  {
    const size_t startID = (threadID+0)*numPrimitives/numThreads;
    const size_t endID   = (threadID+1)*numPrimitives/numThreads;

    PrimRef*  __restrict bptr    = prims + startID;
    Triangle1mc*  __restrict acc = (Triangle1mc*)accel + startID;

    for (size_t j=startID; j<endID; j++, bptr++,acc++)
      {
	prefetch<PFHINT_NTEX>(bptr + L1_PREFETCH_ITEMS);
	prefetch<PFHINT_L2EX>(bptr + L2_PREFETCH_ITEMS);
	assert(bptr->geomID() < scene->size() );

	int geomID = bptr->geomID();
	int primID = bptr->primID();

	const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);

	assert(primID < mesh->size() );

	const TriangleMesh::Triangle & tri = mesh->triangle(primID);

	Vec3fa *vptr0 = (Vec3fa*)mesh->vertexPtr(tri.v[0]);
	Vec3fa *vptr1 = (Vec3fa*)mesh->vertexPtr(tri.v[1]);
	Vec3fa *vptr2 = (Vec3fa*)mesh->vertexPtr(tri.v[2]);

	acc->v0 = vptr0;
	acc->v1 = vptr1;
	acc->v2 = vptr2;
	acc->geometryID  = geomID;
	acc->primitiveID = primID;
      }
  }

  std::string BVH4iBuilderMemoryConservative::getStatistics()
  {
    return BVH4iStatistics<BVH4i::QuantizedNode>(bvh).str();
  }

  /* =================================================================================== */
  /* =================================================================================== */
  /* =================================================================================== */
#define DBG_CACHE_BUILDER(x) 

  void BVH4iBuilderSubdivMesh::printBuilderName()
  {
    std::cout << "building BVH4i with Subdivision Surface SAH builder (MIC) ... " << std::endl;    
  }

  void BVH4iBuilderSubdivMesh::build(const size_t threadIndex, const size_t threadCount)
  {
    DBG_CACHE_BUILDER( PING );

    leafItemThreshold = 1;

    fastUpdateMode = true;
    fastUpdateMode_numFaces = 0;

      /* initialize all half edge structures */
    new (&iter) Scene::Iterator<SubdivMesh>(this->scene);

    DBG_CACHE_BUILDER( PRINT( iter.size() ) );

    for (size_t i=0; i<iter.size(); i++)
      if (iter[i]) 
	{
	  DBG_CACHE_BUILDER( PRINT( i ) );
	  DBG_CACHE_BUILDER( PRINT( iter[i] ) );

	  iter[i]->initializeHalfEdgeStructures();
	  fastUpdateMode_numFaces += iter[i]->size();
	  if (!iter[i]->checkLevelUpdate()) fastUpdateMode = false;
	}
    DBG_CACHE_BUILDER( PRINT( fastUpdateMode ) );

    pstate.init(iter,size_t(1024));

      /* deactivate fast update mode */
    if (numPrimitives == 0 || 
	numPrimitives != fastUpdateMode_numFaces ||
	bvh->root     == BVH4i::emptyNode ||
	bvh->qbvh     == nullptr)
      {
	fastUpdateMode = false;
      }

    if (!fastUpdateMode)
      BVH4iBuilder::build(threadIndex,threadCount);
    else
      {
	TIMER(double msec = getSeconds());
	/* recalculate list of primrefs */
	global_bounds.reset();
	computePrimRefs(threadIndex,threadCount);
	TIMER(msec = getSeconds()-msec);    
	TIMER(std::cout << "task_computePrimRefs " << 1000. * msec << " ms" << std::endl << std::flush);

	/* initialize atomic node counter */
	atomicID.reset(0);

	/* reset numSubTrees */
	numSubTrees = 0;

	/* update BVH4i */
	bvh->bounds = global_bounds.geometry;

	TIMER(msec = getSeconds());	
#if 1
	extract_refit_subtrees(bvh->root,0);
	scene->lockstep_scheduler.dispatchTask( task_refitSubTrees, this, threadIndex, threadCount );	
	refit_top_level(bvh->root,0);
#else	
	refit(bvh->root);	
#endif
	TIMER(msec = getSeconds()-msec);    
	TIMER(std::cout << "refit " << 1000. * msec << " ms" << std::endl << std::flush);

      }
  }


  void BVH4iBuilderSubdivMesh::allocateData(const size_t threadCount, const size_t totalNumPrimitives)
  {
    size_t numPrimitivesOld = numPrimitives;
    numPrimitives = totalNumPrimitives;
    if (numPrimitivesOld != numPrimitives)
      {
	const size_t numPrims = numPrimitives+4;
	const size_t minAllocNodes =  ALLOCATOR_NODE_BLOCK_SIZE * MAX_MIC_THREADS; // (threadCount+1) 
	const size_t numNodes = (size_t)((float)(numPrims+2)/2) + minAllocNodes;
	allocateMemoryPools(numPrims,numNodes,sizeof(BVH4i::Node),sizeof(SubdivPatch1),1.0f);
      }
  }


  size_t BVH4iBuilderSubdivMesh::getNumPrimitives()
  {
    /* in fast update mode we know the number of primitives in advance */
    if (fastUpdateMode) return fastUpdateMode_numFaces;

    PrimInfo pinfo = parallel_for_for_prefix_sum( pstate, iter, PrimInfo(empty), [&](SubdivMesh* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo
     {
       size_t s = 0;
       for (size_t f=r.begin(); f!=r.end(); ++f) 
	{          
          if (!mesh->valid(f)) continue;
	  feature_adaptive_subdivision_gregory(f,mesh->getHalfEdge(f),mesh->getVertexBuffer(),
					       [&](const CatmullClarkPatch& patch, const Vec2f uv[4], const int subdiv[4])
					       {
						 s++;
					       });	    
	}
       return PrimInfo(s,empty,empty);
     }, [](const PrimInfo& a, const PrimInfo b) -> PrimInfo { return PrimInfo(a.size()+b.size(),empty,empty); });

    return pinfo.size();
    /* count total number of subdivision surface objects */
    // size_t numFaces = 0;       
    // for (size_t i=0;i<scene->size();i++)
    //   {
    // 	if (unlikely(scene->get(i) == nullptr)) continue;
    // 	if (unlikely((scene->get(i)->type != Geometry::SUBDIV_MESH) /*&& (scene->get(i)->type != INSTANCES)*/)) continue;
    // 	if (unlikely(!scene->get(i)->isEnabled())) continue;
    //     SubdivMesh* geom = (SubdivMesh*) scene->get(i);
    // 	numFaces += geom->size();
    //   }
    // return numFaces;	
  }


#define FORCE_TESSELLATION_BOUNDS 1

    BBox3fa getBounds(const SubdivPatch1Base &p,const SubdivMesh* const mesh)
    {
      
#if FORCE_TESSELLATION_BOUNDS == 1
       
      __aligned(64) float u_array[(p.grid_size_simd_blocks + 1) * 16]; // +16 for unaligned access
      __aligned(64) float v_array[(p.grid_size_simd_blocks + 1) * 16]; // +16 for unaligned access

      const unsigned int real_grid_size = p.grid_u_res*p.grid_v_res;
      gridUVTessellator(p.level, p.grid_u_res, p.grid_v_res, u_array, v_array);

      if (unlikely(p.needsStitching()))
        stitchUVGrid(p.level, p.grid_u_res, p.grid_v_res, u_array, v_array);

      // FIXME: remove
      for (size_t i = real_grid_size; i<p.grid_size_simd_blocks * 16; i++)
          {
            u_array[i] = 1.0f;
            v_array[i] = 1.0f;
          }
      
      BBox3fa b(empty);
      assert(p.grid_size_simd_blocks >= 1);

      for (size_t i = 0; i<p.grid_size_simd_blocks; i++)
        {
          const mic_f u = load16f(&u_array[i * 16]);
          const mic_f v = load16f(&v_array[i * 16]);

          mic3f vtx = p.eval16(u, v);


          /* eval displacement function */
          if (unlikely(mesh->displFunc != nullptr))
            {
              mic3f normal = p.normal16(u, v);
              normal = normalize(normal);

              const Vec2f uv0 = p.getUV(0);
              const Vec2f uv1 = p.getUV(1);
              const Vec2f uv2 = p.getUV(2);
              const Vec2f uv3 = p.getUV(3);

              const mic_f patch_uu = bilinear_interpolate(uv0.x, uv1.x, uv2.x, uv3.x, u, v);
              const mic_f patch_vv = bilinear_interpolate(uv0.y, uv1.y, uv2.y, uv3.y, u, v);

              mesh->displFunc(mesh->userPtr,
                              p.geom,
                              p.prim,
                              (const float*)&patch_uu,
                              (const float*)&patch_vv,
                              (const float*)&normal.x,
                              (const float*)&normal.y,
                              (const float*)&normal.z,
                              (float*)&vtx.x,
                              (float*)&vtx.y,
                              (float*)&vtx.z,
                              16);

            }

          /* extend bounding box */
          b.extend(getBBox3fa(vtx));
        }

      b.lower.a = 0.0f;
      b.upper.a = 0.0f;

#if defined(DEBUG)
      isfinite(b.lower.x);
      isfinite(b.lower.y);
      isfinite(b.lower.z);

      isfinite(b.upper.x);
      isfinite(b.upper.y);
      isfinite(b.upper.z);
#endif

#else
      BBox3fa b = patch.bounds();
      if (unlikely(isGregoryPatch()))
        {
          b.extend(GregoryPatch::extract_f_m_Vec3fa(patch.v, 0));
          b.extend(GregoryPatch::extract_f_m_Vec3fa(patch.v, 1));
          b.extend(GregoryPatch::extract_f_m_Vec3fa(patch.v, 2));
          b.extend(GregoryPatch::extract_f_m_Vec3fa(patch.v, 3));
        }
#endif

#if defined(_MSC_VER) && !defined(__INTEL_COMPILER)
      _freea(ptr);
#endif

      return b;
    }

  void BVH4iBuilderSubdivMesh::updatePatchTessellation(const size_t threadID, const size_t numThreads) 
  {
    /* count total number of virtual objects */
    const size_t startID   = (threadID+0)*numPrimitives/numThreads;
    const size_t endID     = (threadID+1)*numPrimitives/numThreads; 
    SubdivPatch1 *subdiv_patches = (SubdivPatch1*)accel;

    mic_f bounds_scene_min((float)pos_inf);
    mic_f bounds_scene_max((float)neg_inf);
    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);

    for (size_t i=startID;i<endID;i++)
      {
	SubdivPatch1 &subdiv_patch = subdiv_patches[i];
	prefetch<PFHINT_L1EX>(&subdiv_patch);
	prefetch<PFHINT_L1EX>(&prims[i]);

	prefetch<PFHINT_L2EX>(&subdiv_patch + 2);
	prefetch<PFHINT_L2EX>(&prims[i+2]);

	const SubdivMesh* const mesh = (SubdivMesh*) scene->get(subdiv_patch.geom);
	const SubdivMesh::HalfEdge* first_half_edge = mesh->getHalfEdge(subdiv_patch.prim);

	float edge_level[4] = {
	  first_half_edge[0].edge_level,
	  first_half_edge[1].edge_level,
	  first_half_edge[2].edge_level,
	  first_half_edge[3].edge_level
	};
 
	subdiv_patch.updateEdgeLevels(edge_level,mesh);
	subdiv_patch.resetRootRef();
	const BBox3fa bounds = getBounds(subdiv_patch,mesh);

	assert(bounds.lower.x <= bounds.upper.x);
	assert(bounds.lower.y <= bounds.upper.y);
	assert(bounds.lower.z <= bounds.upper.z);

	const mic_f bmin = broadcast4to16f(&bounds.lower); 
	const mic_f bmax = broadcast4to16f(&bounds.upper);
          
	bounds_scene_min = min(bounds_scene_min,bmin);
	bounds_scene_max = max(bounds_scene_max,bmax);
	const mic_f centroid2 = bmin+bmax;
	bounds_centroid_min = min(bounds_centroid_min,centroid2);
	bounds_centroid_max = max(bounds_centroid_max,centroid2);

						     
	prims[i] = PrimRef(bounds,i);
      }

    Centroid_Scene_AABB bounds;
    
    store4f(&bounds.centroid2.lower,bounds_centroid_min);
    store4f(&bounds.centroid2.upper,bounds_centroid_max);
    store4f(&bounds.geometry.lower,bounds_scene_min);
    store4f(&bounds.geometry.upper,bounds_scene_max);

    global_bounds.extend_atomic(bounds);    
  }

  void BVH4iBuilderSubdivMesh::computePrimRefs(const size_t threadIndex, const size_t threadCount)
  {
#if 1
    if (fastUpdateMode)
      {
	scene->lockstep_scheduler.dispatchTask( task_updatePatchTessellation, this, threadIndex, threadCount );	
	return;
      }
#endif

    PrimInfo pinfo( empty );
    SubdivPatch1 *subdiv_patches = (SubdivPatch1*)accel;

      pinfo = parallel_for_for_prefix_sum( pstate, iter, PrimInfo(empty), [&](SubdivMesh* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo
      {
        PrimInfo s(empty);
        for (size_t f=r.begin(); f!=r.end(); ++f) 
	{
          if (!mesh->valid(f)) continue;

	  if (!fastUpdateMode) 
	    {
	      feature_adaptive_subdivision_gregory(
						   f,mesh->getHalfEdge(f),mesh->getVertexBuffer(),
						   [&](const CatmullClarkPatch& ipatch, const Vec2f uv[4], const int subdiv[4])
						   {
						     float edge_level[4] = {
						       ipatch.ring[0].edge_level,
						       ipatch.ring[1].edge_level,
						       ipatch.ring[2].edge_level,
						       ipatch.ring[3].edge_level
						     };

						     for (size_t i=0;i<4;i++)
						       edge_level[i] = adjustDiscreteTessellationLevel(edge_level[i],subdiv[i]);

						     const unsigned int patchIndex = base.size()+s.size();
						     subdiv_patches[patchIndex] = SubdivPatch1(ipatch, mesh->id, f, mesh, uv, edge_level);
						   
						     /* compute patch bounds */
						     const BBox3fa bounds = getBounds(subdiv_patches[patchIndex],mesh);
						     assert(bounds.lower.x <= bounds.upper.x);
						     assert(bounds.lower.y <= bounds.upper.y);
						     assert(bounds.lower.z <= bounds.upper.z);
						     
						     prims[base.size()+s.size()] = PrimRef(bounds,patchIndex);
						     s.add(bounds);
						   });
	    }
	  else
	    {
	      const unsigned int patchIndex = base.size()+s.size();
	      const SubdivMesh::HalfEdge* first_half_edge = mesh->getHalfEdge(f);
	      float edge_level[4] = {
		first_half_edge[0].edge_level,
		first_half_edge[1].edge_level,
		first_half_edge[2].edge_level,
		first_half_edge[3].edge_level
	      };
	      prefetch<PFHINT_L1EX>(&prims[patchIndex]);
	      prefetch<PFHINT_L2EX>(&prims[patchIndex+16]);
 
	      subdiv_patches[patchIndex].updateEdgeLevels(edge_level,mesh);
	      subdiv_patches[patchIndex].resetRootRef();
	      const BBox3fa bounds = getBounds(subdiv_patches[patchIndex],mesh);
	      assert(bounds.lower.x <= bounds.upper.x);
	      assert(bounds.lower.y <= bounds.upper.y);
	      assert(bounds.lower.z <= bounds.upper.z);
						     
	      prims[patchIndex] = PrimRef(bounds,patchIndex);
	      s.add(bounds);	      
	    }
        }
        return s;
      }, [](PrimInfo a, const PrimInfo& b) -> PrimInfo { a.merge(b); return a; });

      global_bounds.centroid2 = pinfo.centBounds;
      global_bounds.geometry  = pinfo.geomBounds;

  }

  void BVH4iBuilderSubdivMesh::createAccel(const size_t threadIndex, const size_t threadCount)
  {
  }


  /* FIXME: code is deprecated */
  void BVH4iBuilderSubdivMesh::computePrimRefsSubdivMesh(const size_t threadID, const size_t numThreads) 
  {
    const size_t numTotalGroups = scene->size();


    /* count total number of virtual objects */
    const size_t numFaces  = numPrimitives;
    const size_t startID   = (threadID+0)*numFaces/numThreads;
    const size_t endID     = (threadID+1)*numFaces/numThreads; 
    
    PrimRef *__restrict__ const prims     = this->prims;

    // === find first group containing startID ===
    unsigned int g=0, numSkipped = 0;
    for (; g<numTotalGroups; g++) {       
      if (unlikely(scene->get(g) == nullptr)) continue;
      if (unlikely((scene->get(g)->type != Geometry::SUBDIV_MESH) /*&& (scene->get(g)->type != INSTANCES)*/)) continue;
      if (unlikely(!scene->get(g)->isEnabled())) continue;
      const SubdivMesh* const geom = (SubdivMesh*) scene->get(g);
      const size_t numPrims = geom->size();
      if (numSkipped + numPrims > startID) break;
      numSkipped += numPrims;
    }

    /* start with first group containing startID */
    mic_f bounds_scene_min((float)pos_inf);
    mic_f bounds_scene_max((float)neg_inf);
    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);

    unsigned int num = 0;
    unsigned int currentID = startID;
    unsigned int offset = startID - numSkipped;

    SubdivPatch1 *acc = (SubdivPatch1*)accel;

    for (; g<numTotalGroups; g++) 
      {
	if (unlikely(scene->get(g) == nullptr)) continue;
	if (unlikely((scene->get(g)->type != Geometry::SUBDIV_MESH ) /*&& (scene->get(g)->type != INSTANCES)*/)) continue;
	if (unlikely(!scene->get(g)->isEnabled())) continue;

	SubdivMesh *subdiv_mesh = (SubdivMesh *)scene->get(g);

        size_t N = subdiv_mesh->size();
        for (unsigned int i=offset; i<N && currentID < endID; i++, currentID++)	 
	  { 		
	    assert( currentID < numFaces );

	    prefetch<PFHINT_L2EX>(&prims[currentID]);

	    const CatmullClarkPatch ipatch ( subdiv_mesh->getHalfEdge(i),
					     subdiv_mesh->getVertexBuffer() );
	    
            Vec2f uv[4];
            uv[0] = Vec2f(0.0f,0.0f);
            uv[1] = Vec2f(0.0f,1.0f);
            uv[2] = Vec2f(1.0f,1.0f);
            uv[3] = Vec2f(1.0f,0.0f);

            float edge_level[4] = {
              ipatch.ring[0].edge_level,
              ipatch.ring[1].edge_level,
              ipatch.ring[2].edge_level,
              ipatch.ring[3].edge_level
            };

	    SubdivPatch1 tmp = SubdivPatch1(ipatch,
					    g,
					    i,
					    subdiv_mesh,
                                            uv,
                                            edge_level);
	    	    
	    const BBox3fa bounds = getBounds(tmp,subdiv_mesh);
	    
	    tmp.store(&acc[currentID]);

	    prefetch<PFHINT_L1EX>(&prims[currentID]);

	    const mic_f bmin = broadcast4to16f(&bounds.lower); 
	    const mic_f bmax = broadcast4to16f(&bounds.upper);
          
	    bounds_scene_min = min(bounds_scene_min,bmin);
	    bounds_scene_max = max(bounds_scene_max,bmax);
	    const mic_f centroid2 = bmin+bmax;
	    bounds_centroid_min = min(bounds_centroid_min,centroid2);
	    bounds_centroid_max = max(bounds_centroid_max,centroid2);

	    store4f(&prims[currentID].lower,bmin);
	    store4f(&prims[currentID].upper,bmax);	
	    prims[currentID].lower.a = currentID;
	    prims[currentID].upper.a = 0;
	  }
        if (currentID == endID) break;
        offset = 0;
      }

    /* update global bounds */
    Centroid_Scene_AABB bounds;
    
    store4f(&bounds.centroid2.lower,bounds_centroid_min);
    store4f(&bounds.centroid2.upper,bounds_centroid_max);
    store4f(&bounds.geometry.lower,bounds_scene_min);
    store4f(&bounds.geometry.upper,bounds_scene_max);

    global_bounds.extend_atomic(bounds);    
  }


  void BVH4iBuilderSubdivMesh::updateLeaves(const size_t threadIndex, const size_t threadCount)
  {    
    const size_t nodes  = atomicID/2;

    const size_t startID   = (threadIndex+0)*nodes/threadCount;
    const size_t endID     = (threadIndex+1)*nodes/threadCount; 

    for (size_t j=startID; j<endID; j++)
      {
	BVH4i::Node *n = (BVH4i::Node*)&node[j*2];

	for (size_t i=0;i<4;i++)
	  {
	    if (n->child(i) == BVH4i::invalidNode) break;
	    BVH4i::NodeRef &ref = n->child(i);
	    if (ref.isLeaf())
	      {
		unsigned int items = ref.items();
		unsigned int index = ref.offsetIndex();
		if (items == 1 && index < numPrimitives)
		  prefetch<PFHINT_NT>(&prims[index]);
	      }
	  }

	for (size_t i=0;i<4;i++)
	  {
	    if (n->child(i) == BVH4i::invalidNode) break;

	    BVH4i::NodeRef &ref = n->child(i);
	    if (ref.isLeaf())
	      {
		unsigned int items = ref.items();
		unsigned int index = ref.offsetIndex();
		if (items == 1 && index < numPrimitives)
		  {
		    const unsigned int patchIndex = prims[index].lower.a;

		    createBVH4iLeaf( ref , patchIndex, 1);	    
		    assert( n->child(i).isLeaf() );
		    assert( n->child(i).items() == 1 );
		  }
	      }
	  }
      }
  }

  void BVH4iBuilderSubdivMesh::finalize(const size_t threadIndex, const size_t threadCount)
  {    

    scene->lockstep_scheduler.dispatchTask( task_updateLeaves, this, threadIndex, threadCount );   

    if (threadIndex == 0)
      {
	bvh->accel         = accel;
	bvh->numPrimitives = numPrimitives;
      }

  }

  BBox3fa BVH4iBuilderSubdivMesh::refit(const BVH4i::NodeRef &ref)
  {    
    if (unlikely(ref.isLeaf()))
      {
	const unsigned int patchIndex = ref.offsetIndex();
	return prims[patchIndex].bounds();
      }

    BVH4i::Node *n = (BVH4i::Node*)ref.node(node);

    BBox3fa parentBounds = empty;

    for (size_t i=0;i<BVH4i::N;i++)
      {
	if (n->child(i) == BVH4i::invalidNode) break;
	
	if (n->child(i).isLeaf())
	  {
	    parentBounds.extend( n->bounds(i) );
	  }
	else
	  {
	    BBox3fa bounds = refit( n->child(i) );

	    n->setBounds(i,bounds);
	    parentBounds.extend( bounds );
	  }
      }
    return parentBounds;
  }    

  void BVH4iBuilderSubdivMesh::extract_refit_subtrees(const BVH4i::NodeRef &ref,const size_t depth)
  {
    if (depth == GENERATE_SUBTREES_MAX_TREE_DEPTH || ref.isLeaf())
      {
	assert(numSubTrees < MAX_SUBTREES);
	subtree_refs[numSubTrees++] = ref;
	return;
      }

    BVH4i::Node *n = (BVH4i::Node*)ref.node(node);

    for (size_t i=0;i<4;i++)
      {
	if (n->child(i) == BVH4i::invalidNode) break;
	extract_refit_subtrees(n->child(i),depth+1);
      }
  }


  BBox3fa BVH4iBuilderSubdivMesh::refit_top_level(const BVH4i::NodeRef &ref, 
						  const size_t depth)
  {    
    if (unlikely(ref.isLeaf()))
      {
	const unsigned int patchIndex = ref.offsetIndex();
	return prims[patchIndex].bounds();
      }

    BVH4i::Node *n = (BVH4i::Node*)ref.node(node);

    if (depth == GENERATE_SUBTREES_MAX_TREE_DEPTH)
      {
	BBox3fa parentBounds = empty;
	for (size_t i=0;i<4;i++)
	  {
	    if (n->child(i) == BVH4i::invalidNode) break;
	    parentBounds.extend( n->bounds(i) );
	  }
	return parentBounds;
      }

    BBox3fa parentBounds = empty;

    for (size_t i=0;i<BVH4i::N;i++)
      {
	if (n->child(i) == BVH4i::invalidNode) break;
	
	if (n->child(i).isLeaf())
	  {
	    parentBounds.extend( n->bounds(i) );
	  }
	else
	  {
	    BBox3fa bounds = refit_top_level( n->child(i),depth+1 );

	    n->setBounds(i,bounds);
	    parentBounds.extend( bounds );
	  }
      }
    return parentBounds;
  }    

  void BVH4iBuilderSubdivMesh::refitSubTrees(const size_t threadID, const size_t numThreads) 
  {
    while(1)
      {
	unsigned int ID = atomicID.inc();
	if (ID >= numSubTrees) break;
	const BVH4i::NodeRef &ref = subtree_refs[ID];
	BBox3fa bounds = refit(ref);
      }
    
  }
};
