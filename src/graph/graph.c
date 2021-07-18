/*
* Copyright 2018-2021 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "RG.h"
#include "graph.h"
#include "../util/arr.h"
#include "../util/qsort.h"
#include "../util/rmalloc.h"
#include "../configuration/config.h"
#include "../util/datablock/oo_datablock.h"

static GrB_BinaryOp _graph_edge_accum = NULL;
// GraphBLAS binary operator for freeing edges
static GrB_BinaryOp _binary_op_delete_edges = NULL;

//------------------------------------------------------------------------------
// Forward declarations
//------------------------------------------------------------------------------
void _MatrixResizeToCapacity(const Graph *g, RG_Matrix m);

//------------------------------------------------------------------------------
// GraphBLAS functions
//------------------------------------------------------------------------------
void _edge_accum(void *_z, const void *_x, const void *_y) {
	EdgeID *z = (EdgeID *)_z;
	const EdgeID *x = (const EdgeID *)_x;
	const EdgeID *y = (const EdgeID *)_y;

	EdgeID *ids;
	/* Single edge ID,
	 * switching from single edge ID to multiple IDs. */
	if(SINGLE_EDGE(*x)) {
		ids = array_new(EdgeID, 2);
		array_append(ids, *x);
		array_append(ids, *y);
		*z = (EdgeID)SET_MSB(ids);
	} else {
		// Multiple edges, adding another edge.
		ids = CLEAR_MSB(*x);
		array_append(ids, *y);
		*z = (EdgeID)SET_MSB(ids);
	}
}

void _binary_op_free_edge(void *z, const void *x, const void *y) {
	const Graph *g = (const Graph *) * ((uint64_t *)x);
	const EdgeID *id = (const EdgeID *)y;

	if((SINGLE_EDGE(*id))) {
		DataBlock_DeleteItem(g->edges, *id);
	} else {
		EdgeID *ids = CLEAR_MSB(*id);
		uint id_count = array_len(ids);
		for(uint i = 0; i < id_count; i++) {
			DataBlock_DeleteItem(g->edges, ids[i]);
		}
		array_free(ids);
	}
}

//------------------------------------------------------------------------------
// Synchronization functions 
//------------------------------------------------------------------------------

// acquire a lock that does not restrict access from additional reader threads
void Graph_AcquireReadLock(Graph *g) {
	pthread_rwlock_rdlock(&g->_rwlock);
}

// acquire a lock for exclusive access to this graph's data
void Graph_AcquireWriteLock(Graph *g) {
	pthread_rwlock_wrlock(&g->_rwlock);
	g->_writelocked = true;
}

// Release the held lock
void Graph_ReleaseLock
(
	Graph *g
) {
	// set _writelocked to false BEFORE unlocking
	// if this is a reader thread no harm done,
	// if this is a writer thread the writer is about to unlock so once again
	// no harm done, if we set `_writelocked` to false after unlocking it is possible
	// for a reader thread to be considered as writer, performing illegal access to
	// underline matrices, consider a context switch after unlocking `_rwlock` but
	// before setting `_writelocked` to false
	g->_writelocked = false;
	pthread_rwlock_unlock(&g->_rwlock);
}

//------------------------------------------------------------------------------
// Graph utility functions
//------------------------------------------------------------------------------

// return number of nodes graph can contain
static inline size_t _Graph_NodeCap(const Graph *g) {
	return g->nodes->itemCap;
}

// return number of edges graph can contain
static inline size_t _Graph_EdgeCap(const Graph *g) {
	return g->edges->itemCap;
}

static void _CollectEdgesFromEntry
(
	const Graph *g,
	NodeID src,
	NodeID dest,
	int r,
	EdgeID edgeId,
	Edge **edges
) {
	Edge e = {0};

	e.relationID  =  r;
	e.srcNodeID   =  src;
	e.destNodeID  =  dest;

	if(SINGLE_EDGE(edgeId)) {
		e.id      =  edgeId;
		e.entity  =  DataBlock_GetItem(g->edges,  edgeId);
		ASSERT(e.entity);
		array_append(*edges, e);
	} else {
		// multiple edges connecting src to dest,
		// entry is a pointer to an array of edge IDs
		EdgeID *edgeIds = (intptr_t)CLEAR_MSB(edgeId);
		int edgeCount = array_len(edgeIds);

		for(int i = 0; i < edgeCount; i++) {
			edgeId = edgeIds[i];
			e.entity = DataBlock_GetItem(g->edges, edgeId);
			e.id = edgeId;
			ASSERT(e.entity);
			array_append(*edges, e);
		}
	}
}

// Locates edges connecting src to destination.
void _Graph_GetEdgesConnectingNodes
(
	const Graph *g,
	NodeID src,
	NodeID dest,
	int r,
	Edge **edges
) {
	ASSERT(g);
	ASSERT(r    != GRAPH_NO_RELATION);
	ASSERT(r    < Graph_RelationTypeCount(g));
	ASSERT(src  < Graph_RequiredMatrixDim(g));
	ASSERT(dest < Graph_RequiredMatrixDim(g));

	// relation map, maps (src, dest, r) to edge IDs.
	EdgeID      id    =  INVALID_ENTITY_ID;
	GrB_Matrix  M     =  Graph_GetRelationMatrix(g, r, false);
	GrB_Info    res   =  GrB_Matrix_extractElement_UINT64(&id, M, src, dest);

	// no entry at [dest, src], src is not connected to dest with relation R
	if(res == GrB_NO_VALUE) return;

	_CollectEdgesFromEntry(g, src, dest, r, id, edges);
}

static inline Entity *_Graph_GetEntity(const DataBlock *entities, EntityID id) {
	return DataBlock_GetItem(entities, id);
}

//------------------------------------------------------------------------------
// Matrix synchronization and resizing functions
//------------------------------------------------------------------------------

// resize given matrix, such that its number of row and columns
// matches the number of nodes in the graph. Also, synchronize
// matrix to execute any pending operations
void _MatrixSynchronize
(
	const Graph *g,
	RG_Matrix m
) {
	bool dirty = RG_Matrix_isDirty(m);
	GrB_Info info;
	GrB_Index n_rows;
	GrB_Index n_cols;
	RG_Matrix_nrows(&n_rows, m);
	RG_Matrix_ncols(&n_cols, m);
	GrB_Index dims = Graph_RequiredMatrixDim(g);

	// matrix must be resized if its dimensions missmatch required dimensions
	bool require_resize = (n_rows != dims || n_cols != dims);

	// matrix fully synced, nothing to do
	if(!require_resize && !RG_Matrix_isDirty(m)) return;

	//--------------------------------------------------------------------------
	// sync under WRITE
	//--------------------------------------------------------------------------

	// if the graph belongs to one thread, we don't need to lock the mutex
	if(g->_writelocked) {
		if(require_resize) {
			info = RG_Matrix_resize(m, dims, dims);
			ASSERT(info == GrB_SUCCESS);
		}

		// writer under write lock, no need to flush pending changes
		return;
	}

	//--------------------------------------------------------------------------
	// sync under READ
	//--------------------------------------------------------------------------

	// lock the matrix
	RG_Matrix_Lock(m);

	// recheck
	RG_Matrix_nrows(&n_rows, m);
	RG_Matrix_ncols(&n_cols, m);
	dims = Graph_RequiredMatrixDim(g);
	require_resize = (n_rows != dims || n_cols != dims);

	// some other thread performed sync
	if(!require_resize && !RG_Matrix_isDirty(m)) goto cleanup;

	// resize if required
	if(require_resize) {
		info = RG_Matrix_resize(m, dims, dims);
		ASSERT(info == GrB_SUCCESS);
	}

	// flush pending changes if dirty
	if(RG_Matrix_isDirty(m)) {
		info = RG_Matrix_wait(m, false);
		ASSERT(info == GrB_SUCCESS);
	}

cleanup:
	// Unlock matrix mutex.
	RG_Matrix_Unlock(m);
}

// resize matrix to node capacity
void _MatrixResizeToCapacity
(
	const Graph *g,
	RG_Matrix m
) {
	GrB_Index nrows;
	GrB_Index ncols;
	RG_Matrix_ncols(&ncols, m);
	RG_Matrix_nrows(&nrows, m);
	GrB_Index cap = Graph_RequiredMatrixDim(g);

	// this policy should only be used in a thread-safe context,
	// so no locking is required
	if(nrows != cap || ncols != cap) {
		GrB_Info res = RG_Matrix_resize(m, cap, cap);
		ASSERT(res == GrB_SUCCESS);
	}
}

// do not update matrices
void _MatrixNOP
(
	const Graph *g,
	RG_Matrix matrix
) {
	return;
}

// define the current behavior for matrix creations and retrievals on this graph
void Graph_SetMatrixPolicy
(
	Graph *g,
	MATRIX_POLICY policy
) {
	switch(policy) {
		case SYNC_AND_MINIMIZE_SPACE:
			// Default behavior; forces execution of pending GraphBLAS operations
			// when appropriate and sizes matrices to the current node count.
			g->SynchronizeMatrix = _MatrixSynchronize;
			break;
		case RESIZE_TO_CAPACITY:
			// Bulk insertion and creation behavior; does not force pending operations
			// and resizes matrices to the graph's current node capacity.
			g->SynchronizeMatrix = _MatrixResizeToCapacity;
			break;
		case DISABLED:
			// Used when deleting or freeing a graph; forces no matrix updates or resizes.
			g->SynchronizeMatrix = _MatrixNOP;
			break;
		default:
			ASSERT(false);
	}
}

// synchronize and resize all matrices in graph
void Graph_ApplyAllPending
(
	Graph *g
) {
	RG_Matrix M;

	M = g->adjacency_matrix;
	g->SynchronizeMatrix(g, M);

	for(int i = 0; i < array_len(g->labels); i ++) {
		M = g->labels[i];
		g->SynchronizeMatrix(g, M);
	}

	for(int i = 0; i < array_len(g->relations); i ++) {
		M = g->relations[i];
		g->SynchronizeMatrix(g, M);
	}
}

//------------------------------------------------------------------------------
// Graph API
//------------------------------------------------------------------------------

Graph *Graph_New
(
	size_t node_cap,
	size_t edge_cap
) {
	node_cap = MAX(node_cap, GRAPH_DEFAULT_NODE_CAP);
	edge_cap = MAX(node_cap, GRAPH_DEFAULT_EDGE_CAP);

	Graph *g = rm_calloc(1, sizeof(Graph));

	g->nodes      =  DataBlock_New(node_cap,  sizeof(Entity), (fpDestructor)FreeEntity);
	g->edges      =  DataBlock_New(edge_cap,  sizeof(Entity), (fpDestructor)FreeEntity);
	g->labels     =  array_new(RG_Matrix,     GRAPH_DEFAULT_LABEL_CAP);
	g->relations  =  array_new(RG_Matrix,     GRAPH_DEFAULT_RELATION_TYPE_CAP);

	GrB_Info info;
	UNUSED(info);

	GrB_Index n = Graph_RequiredMatrixDim(g);
	RG_Matrix_new(&g->adjacency_matrix, GrB_BOOL, n, n, false, true);
	RG_Matrix_new(&g->_zero_matrix, GrB_BOOL, n, n, false, false);

	// init graph statistics
	GraphStatistics_init(&g->stats);

	// initialize a read-write lock scoped to the individual graph
	int res;
	UNUSED(res);
	res = pthread_rwlock_init(&g->_rwlock, NULL);
	ASSERT(res == 0);
	g->_writelocked = false;

	// force GraphBLAS updates and resize matrices to node count by default
	Graph_SetMatrixPolicy(g, SYNC_AND_MINIMIZE_SPACE);

	// create edge accumulator binary function
	if(!_graph_edge_accum) {
		info = GrB_BinaryOp_new(&_graph_edge_accum, _edge_accum, GrB_UINT64, GrB_UINT64, GrB_UINT64);
		ASSERT(info == GrB_SUCCESS);
	}

	if(!_binary_op_delete_edges) {
		// The binary operator has not yet been constructed; build it now.
		info = GrB_BinaryOp_new(&_binary_op_delete_edges, _binary_op_free_edge,
				GrB_UINT64, GrB_UINT64, GrB_UINT64);
		ASSERT(info == GrB_SUCCESS);
	}

	return g;
}

// All graph matrices are required to be squared NXN
// where N = Graph_RequiredMatrixDim.
inline size_t Graph_RequiredMatrixDim(const Graph *g) {
	return _Graph_NodeCap(g);
}

size_t Graph_NodeCount(const Graph *g) {
	ASSERT(g);
	return g->nodes->itemCount;
}

uint Graph_DeletedNodeCount(const Graph *g) {
	ASSERT(g);
	return DataBlock_DeletedItemsCount(g->nodes);
}

size_t Graph_UncompactedNodeCount(const Graph *g) {
	return Graph_NodeCount(g) + Graph_DeletedNodeCount(g);
}

size_t Graph_LabeledNodeCount
(
	const Graph *g, int label
) {
	RG_Matrix m = Graph_GetLabelMatrix(g, label);
	ASSERT(m);

	GrB_Index nvals = 0;
	RG_Matrix_nvals(&nvals, m);
	return nvals;
}

size_t Graph_EdgeCount(const Graph *g) {
	ASSERT(g);
	return g->edges->itemCount;
}

uint64_t Graph_RelationEdgeCount(const Graph *g, int relation_idx) {
	return GraphStatistics_EdgeCount(&g->stats, relation_idx);
}

uint Graph_DeletedEdgeCount(const Graph *g) {
	ASSERT(g);
	return DataBlock_DeletedItemsCount(g->edges);
}

int Graph_RelationTypeCount(const Graph *g) {
	return array_len(g->relations);
}

int Graph_LabelTypeCount(const Graph *g) {
	return array_len(g->labels);
}

void Graph_AllocateNodes(Graph *g, size_t n) {
	ASSERT(g);
	DataBlock_Accommodate(g->nodes, n);
}

void Graph_AllocateEdges(Graph *g, size_t n) {
	ASSERT(g);
	DataBlock_Accommodate(g->edges, n);
}

int Graph_GetNode
(
	const Graph *g,
	NodeID id,
	Node *n
) {
	ASSERT(g);
	ASSERT(n);
	n->entity = _Graph_GetEntity(g->nodes, id);
	n->id = id;
	return (n->entity != NULL);
}

int Graph_GetEdge
(
	const Graph *g,
	EdgeID id,
	Edge *e
) {
	ASSERT(g);
	ASSERT(e);
	ASSERT(id < _Graph_EdgeCap(g));

	e->id = id;
	e->entity = _Graph_GetEntity(g->edges, id);
	return (e->entity != NULL);
}

int Graph_GetNodeLabel
(
	const Graph *g,
	NodeID nodeID
) {
	ASSERT(g);

	int   label  =  GRAPH_NO_LABEL;
	uint  n      =  array_len(g->labels);

	for(int i = 0; i < n; i++) {
		bool x = false;
		RG_Matrix M = Graph_GetLabelMatrix(g, i);
		GrB_Info res = RG_Matrix_extractElement_BOOL(&x, M, nodeID, nodeID);
		if(res == GrB_SUCCESS) {
			label = i;
			break;
		}
	}

	return label;
}

int Graph_GetEdgeRelation
(
	const Graph *g,
	Edge *e
) {
	ASSERT(g);
	ASSERT(e);

	GrB_Info info;
	int     rel         =  GRAPH_NO_RELATION;
	EdgeID  id          =  ENTITY_GET_ID(e);
	NodeID  srcNodeID   =  Edge_GetSrcNodeID(e);
	NodeID  destNodeID  =  Edge_GetDestNodeID(e);

	// search for relation mapping matrix M, where M[dest,src] == edge ID
	uint n = array_len(g->relations);
	for(uint i = 0; i < n; i++) {
		EdgeID edgeId = 0;
		RG_Matrix M = Graph_GetRelationMatrix(g, i, false);
		info = RG_Matrix_extractElement_UINT64(&edgeId, M, srcNodeID,
				destNodeID);
		if(info != GrB_SUCCESS) continue;

		if(SINGLE_EDGE(edgeId)) {
			EdgeID curEdgeID = edgeId;
			if(curEdgeID == id) {
				Edge_SetRelationID(e, i);
				rel = i;
				break;
			}
		} else {
			// multiple edges exists between src and dest
			// see if given edge is one of them
			EdgeID *edges = CLEAR_MSB(edgeId);
			int edge_count = array_len(edges);
			for(int j = 0; j < edge_count; j++) {
				if(edges[j] == id) {
					Edge_SetRelationID(e, i);
					rel = i;
					break;
				}
			}
		}
	}

	// we must be able to find edge relation
	ASSERT(rel != GRAPH_NO_RELATION);
	return rel;
}

void Graph_GetEdgesConnectingNodes
(
	const Graph *g,
	NodeID srcID,
	NodeID destID,
	int r,
	Edge **edges)
{
	ASSERT(g);
	ASSERT(edges);
	ASSERT(r < Graph_RelationTypeCount(g)); 

	// invalid relation type specified;
	// this can occur on multi-type traversals like:
	// MATCH ()-[:real_type|fake_type]->()
	if(r == GRAPH_UNKNOWN_RELATION) return;

#ifdef RG_DEBUG
	Node  srcNode   =  GE_NEW_NODE();
	Node  destNode  =  GE_NEW_NODE();
	ASSERT(Graph_GetNode(g, srcID, &srcNode));
	ASSERT(Graph_GetNode(g, destID, &destNode));
#endif

	if(r != GRAPH_NO_RELATION) {
		_Graph_GetEdgesConnectingNodes(g, srcID, destID, r, edges);
	} else {
		// relation type missing, scan through each edge type
		int relationCount = Graph_RelationTypeCount(g);
		for(int i = 0; i < relationCount; i++) {
			_Graph_GetEdgesConnectingNodes(g, srcID, destID, i, edges);
		}
	}
}

void Graph_CreateNode
(
	Graph *g,
	int label,
	Node *n
) {
	ASSERT(g);
	ASSERT(n);

	NodeID id;
	Entity *en = DataBlock_AllocateItem(g->nodes, &id);

	n->id           =  id;
	n->entity       =  en;
	n->labelID      =  label;
	en->prop_count  =  0;
	en->properties  =  NULL;

	if(label != GRAPH_NO_LABEL) {
		// set matrix at position [id, id]
		RG_Matrix  m    =  Graph_GetLabelMatrix(g, label);
		GrB_Info   res  =  RG_Matrix_setElement_BOOL(m, true, id, id);
		ASSERT(res == GrB_SUCCESS);
	}
}

void Graph_FormConnection
(
	Graph *g,
	NodeID src,
	NodeID dest,
	EdgeID edge_id,
	int r
) {
	ASSERT(g != NULL);

	GrB_Info info;
	UNUSED(info);
	RG_Matrix  M    =  Graph_GetRelationMatrix(g, r, false);
	RG_Matrix  adj  =  Graph_GetAdjacencyMatrix(g, false);

	// rows represent source nodes, columns represent destination nodes
	info = RG_Matrix_setElement_BOOL(adj, true, src, dest);
	ASSERT(info == GrB_SUCCESS);

	info = RG_Matrix_setElement_UINT64(M, edge_id, src, dest);
	ASSERT(info == GrB_SUCCESS);

	// an edge of type r has just been created, update statistics
	GraphStatistics_IncEdgeCount(&g->stats, r, 1);
}

void Graph_CreateEdge
(
	Graph *g,
	NodeID src,
	NodeID dest,
	int r,
	Edge *e
) {
	ASSERT(g);
	ASSERT(r < Graph_RelationTypeCount(g));

#ifdef RG_DEBUG
	// make sure both src and destination nodes exists
	Node node = GE_NEW_NODE();
	ASSERT(Graph_GetNode(g, src, &node) == 1);
	ASSERT(Graph_GetNode(g, dest, &node) == 1);
#endif

	EdgeID id;
	Entity *en = DataBlock_AllocateItem(g->edges, &id);

	e->id           =  id;
	e->entity       =  en;
	e->srcNodeID    =  src;
	e->destNodeID   =  dest;
	e->relationID   =  r;
	en->prop_count  =  0;
	en->properties  =  NULL;

	Graph_FormConnection(g, src, dest, id, r);
}
// retrieves all either incoming or outgoing edges
// to/from given node N, depending on given direction
void Graph_GetNodeEdges
(
	const Graph *g,      // graph to collect edges from
	const Node *n,       // either source or destination node
	GRAPH_EDGE_DIR dir,  // edge direction ->, <-, <->
	int edgeType,        // relationship type
	Edge **edges         // [output] array of edges
) {
	ASSERT(g);
   	ASSERT(n);
	ASSERT(edges);

	RG_Matrix            M        =  NULL;
	GxB_MatrixTupleIter  *it      =  NULL;
	NodeID               srcID    =  ENTITY_GET_ID(n);
	NodeID               destID   =  INVALID_ENTITY_ID;
	EdgeID               edgeID   =  INVALID_ENTITY_ID;
	bool                 depleted =  false;

	if(edgeType == GRAPH_UNKNOWN_RELATION) return;

	bool outgoing = (dir == GRAPH_EDGE_DIR_OUTGOING ||
			         dir == GRAPH_EDGE_DIR_BOTH);

	bool incoming = (dir == GRAPH_EDGE_DIR_INCOMING ||
		             dir == GRAPH_EDGE_DIR_BOTH);

	if(outgoing) {
		// if a relationship type is specified,
		// retrieve the appropriate relation matrix
		// otherwise use the overall adjacency matrix
		M = Graph_GetRelationMatrix(g, edgeType, false);

		// construct an iterator to traverse over the source node row,
		// containing all outgoing edges
		RG_MatrixTupleIter_new(&it, M);
		RG_MatrixTupleIter_iterate_row(it, srcID);
		while(RG_MatrixTupleIter_next(it, NULL, &destID, &edgeID, &depleted) == GrB_SUCCESS) {
			if(depleted) break;

			// collect all edges (src)->(dest)
			if(edgeType != GRAPH_NO_RELATION) {
				_CollectEdgesFromEntry(g, srcID, destID, edgeType, edgeID, edges);
			} else {
				Graph_GetEdgesConnectingNodes(g, srcID, destID, edgeType, edges);
			}
		}
		RG_MatrixTupleIter_free(it);
	}

	if(incoming) {
		// if a relationship type is specified, retrieve the appropriate
		// transposed relation matrix,
		// otherwise use the transposed adjacency matrix
		M = Graph_GetTransposedRelationMatrix(g, edgeType);

		// construct an iterator to traverse over the source node row,
		// containing all incoming edges
		RG_MatrixTupleIter_new(&it, M);
		RG_MatrixTupleIter_iterate_row(it, srcID);

		while(RG_MatrixTupleIter_next(it, NULL, &destID, &edgeID, &depleted) == GrB_SUCCESS) {
			if(depleted) break;
			// collect all edges connecting destId to srcId
			if(edgeType != GRAPH_NO_RELATION) {
				_CollectEdgesFromEntry(g, destID, srcID, edgeType, edgeID, edges);
			} else {
				Graph_GetEdgesConnectingNodes(g, destID, srcID, edgeType, edges);
			}
		}

		// Clean up
		RG_MatrixTupleIter_free(it);
	}
}

// removes an edge from Graph and updates graph relevent matrices
int Graph_DeleteEdge
(
	Graph *g,
	Edge *e
) {
	ASSERT(g != NULL);
	ASSERT(e != NULL);

	uint64_t    x;
	RG_Matrix   R;
	RG_Matrix   M;
	GrB_Info    info;
	EdgeID      edge_id;
	RG_Matrix   TR        =  GrB_NULL;
	int         r         =  Edge_GetRelationID(e);
	NodeID      src_id    =  Edge_GetSrcNodeID(e);
	NodeID      dest_id   =  Edge_GetDestNodeID(e);

	R = Graph_GetRelationMatrix(g, r, false);

	// test to see if edge exists
	info = RG_Matrix_extractElement_UINT64(&edge_id, R, src_id, dest_id);
	if(info != GrB_SUCCESS) return 0;

	// an edge of type r has just been deleted, update statistics
	GraphStatistics_DecEdgeCount(&g->stats, r, 1);

	if(SINGLE_EDGE(edge_id)) {
		// single edge of type R connecting src to dest, delete entry
		info = RG_Matrix_removeEntry(R, src_id, dest_id, edge_id);
		ASSERT(info == GrB_SUCCESS);

		// see if source is connected to destination with additional edges
		bool connected = false;
		int relationCount = Graph_RelationTypeCount(g);
		for(int i = 0; i < relationCount; i++) {
			if(i == r) continue;
			M = Graph_GetRelationMatrix(g, i, false);
			info = RG_Matrix_extractElement_UINT64(&x, M, src_id, dest_id);
			if(info == GrB_SUCCESS) {
				connected = true;
				break;
			}
		}

		// there are no additional edges connecting source to destination
		// remove edge from THE adjacency matrix
		if(!connected) {
			M = Graph_GetRelationMatrix(g, GRAPH_NO_RELATION, false);
			info = RG_Matrix_removeElement(M, src_id, dest_id);
			ASSERT(info == GrB_SUCCESS);
		}
	} else {
		// multiple edges connecting src to dest
		// locate specific edge and remove it
		// revert back from array representation to edge ID
		// incase we're left with a single edge connecting src to dest

		int i = 0;
		EdgeID id = ENTITY_GET_ID(e);
		EdgeID *edges = CLEAR_MSB(edge_id);
		int edge_count = array_len(edges);

		// locate edge within edge array
		for(; i < edge_count; i++) if(edges[i] == id) break;
		ASSERT(i < edge_count);

		// remove edge from edge array
		// migrate last edge ID and reduce array size
		// TODO: reallocate array of size / capacity ratio is high
		array_del_fast(edges, i);

		// incase we're left with a single edge connecting src to dest
		// revert back from array to scalar
		if(array_len(edges) == 1) {
			edge_id = edges[0];
			array_free(edges);
			RG_Matrix_setElement_UINT64(R, edge_id, src_id, dest_id);
		}
	}

	// free and remove edges from datablock.
	DataBlock_DeleteItem(g->edges, ENTITY_GET_ID(e));
	return 1;
}

inline bool Graph_EntityIsDeleted(Entity *e) {
	return DataBlock_ItemIsDeleted(e);
}

void Graph_DeleteNode
(
	Graph *g,
	Node *n
) {
	// assumption, node is completely detected,
	// there are no incoming nor outgoing edges
	// leading to / from node
	ASSERT(g && n);

	// clear label matrix at position node ID
	uint32_t label_count = array_len(g->labels);
	for(int i = 0; i < label_count; i++) {
		RG_Matrix M = Graph_GetLabelMatrix(g, i);
		RG_Matrix_removeElement(M, ENTITY_GET_ID(n), ENTITY_GET_ID(n));
	}

	DataBlock_DeleteItem(g->nodes, ENTITY_GET_ID(n));
}

static void _Graph_FreeRelationMatrices
(
	const Graph *g
) {
	uint relationCount = Graph_RelationTypeCount(g);
	for(uint i = 0; i < relationCount; i++) RG_Matrix_free(&g->relations[i]);
}

static void _BulkDeleteNodes
(
	Graph *g,
	Node *nodes,
	uint node_count, 
	uint *node_deleted, 
	uint *edge_deleted
) {
	ASSERT(g != NULL);
	ASSERT(nodes != NULL);
	ASSERT(g->_writelocked);
	ASSERT(node_count > 0);

	RG_Matrix           adj;        // adjacency matrix
	RG_Matrix           tadj;       // transposed adjacency matrix
	GrB_Index           nrows;
	GrB_Index           ncols;
	GrB_Index           nvals;       // number of elements in mask
	GxB_MatrixTupleIter *adj_iter;   // adjacency matrix iterator
	GxB_MatrixTupleIter *tadj_iter;  // transposed adjacency matrix iterator

	GrB_Index *implicit_edges = array_new(GrB_Index, 1);

	adj = Graph_GetAdjacencyMatrix(g, false);
	tadj = Graph_GetAdjacencyMatrix(g, true);
	GxB_MatrixTupleIter_new(&adj_iter, adj);
	GxB_MatrixTupleIter_new(&tadj_iter, tadj);

	//--------------------------------------------------------------------------
	// collect edges to delete
	//--------------------------------------------------------------------------

	for(uint i = 0; i < node_count; i++) {
		GrB_Index src;
		GrB_Index dest;
		Node *n = nodes + i;
		bool depleted = false;
		NodeID ID = ENTITY_GET_ID(n);

		//----------------------------------------------------------------------
		// outgoing edges
		//----------------------------------------------------------------------

		GxB_MatrixTupleIter_iterate_row(adj_iter, ID);
		while(true) {
			GxB_MatrixTupleIter_next(adj_iter, NULL,  &dest, NULL, &depleted);
			if(depleted) break;

			// append src followed by dest
			array_append(implicit_edges, ID);
			array_append(implicit_edges, dest); 
		}

		depleted = false;

		//----------------------------------------------------------------------
		// incoming edges
		//----------------------------------------------------------------------

		GxB_MatrixTupleIter_iterate_row(tadj_iter, ID);
		while(true) {
			GxB_MatrixTupleIter_next(tadj_iter, NULL, &src, NULL, &depleted);
			if(depleted) break;

			// append src followed by dest
			array_append(implicit_edges, src);
			array_append(implicit_edges, ID); 
		}
	}
	
	int implicit_edge_count = array_len(implicit_edges);

	// update deleted node count
	*node_deleted += node_count;

	// update deleted edge count
	*edge_deleted += implicit_edge_count;

	//--------------------------------------------------------------------------
	// remove edges from relationship matrices
	//--------------------------------------------------------------------------

	int relation_count = Graph_RelationTypeCount(g);
	for(int i = 0; i < relation_count; i++) {
		RG_Matrix R = Graph_GetRelationMatrix(g, i, false);
		uint64_t edges_before_deletion = Graph_EdgeCount(g);

		for(int j = 0; j < implicit_edge_count; j+=2) {
			GrB_Index src = implicit_edges[j];
			GrB_Index dest = implicit_edges[j+1];
			RG_Matrix_removeElement(R, src, dest);
		}

		// the number of deleted edges is equals to the diff in the number of
		// items in the DataBlock
		uint64_t n_deleted_edges = edges_before_deletion - Graph_EdgeCount(g);

		// multiple edges of type r has just been deleted, update statistics
		GraphStatistics_DecEdgeCount(&g->stats, i, n_deleted_edges);
	}

	//--------------------------------------------------------------------------
	// remove edges from the adjacency matrix
	//--------------------------------------------------------------------------

	for(int j = 0; j < implicit_edge_count; j+=2) {
		GrB_Index src = implicit_edges[j];
		GrB_Index dest = implicit_edges[j+1];
		RG_Matrix_removeElement(adj, src, dest);
	}

	//--------------------------------------------------------------------------
	// remove nodes from label matrices
	//--------------------------------------------------------------------------

	// all nodes marked for deleteion are detected (no incoming/outgoing edges)
	int node_type_count = Graph_LabelTypeCount(g);
	for(int i = 0; i < node_type_count; i++) {
		RG_Matrix L = Graph_GetLabelMatrix(g, i);

		for(int j = 0; j < node_count; j++) {
			Node *n = nodes + j;
			NodeID ID = ENTITY_GET_ID(n);
			RG_Matrix_removeElement(L, ID, ID);
		}
	}

	// clean up
	array_free(implicit_edges);
	GxB_MatrixTupleIter_free(adj_iter);
	GxB_MatrixTupleIter_free(tadj_iter);
}

static void _BulkDeleteEdges(Graph *g, Edge *edges, size_t edge_count) {
	ASSERT(g);
	ASSERT(g->_writelocked);
	ASSERT(edges);
	ASSERT(edge_count > 0);

	bool        update_adj_matrices   =  false;
	int         relationCount         =  Graph_RelationTypeCount(g);

	GrB_Index n = Graph_RequiredMatrixDim(g);

	for(int i = 0; i < edge_count; i++) {
		Edge       *e       =  edges + i;
		int        r        =  Edge_GetRelationID(e);
		RG_Matrix  R        =  Graph_GetRelationMatrix(g, r, false);
		NodeID     src_id   =  Edge_GetSrcNodeID(e);
		NodeID     dest_id  =  Edge_GetDestNodeID(e);
		EdgeID     edge_id  =  ENTITY_GET_ID(e);

		GrB_Info info = RG_Matrix_removeEntry(R, src_id, dest_id, edge_id);
		ASSERT(info == GrB_SUCCESS);

		// edge of type r has just been deleted, update statistics
		GraphStatistics_DecEdgeCount(&g->stats, r, 1);

		if(SINGLE_EDGE(edge_id))  update_adj_matrices = true;

		// free and remove edges from datablock
		DataBlock_DeleteItem(g->edges, ENTITY_GET_ID(e));
	}

	if(update_adj_matrices) {
		// TODO: update adj matrix
		ASSERT(false);
	}
}

/* Removes both nodes and edges from graph. */
void Graph_BulkDelete(Graph *g, Node *nodes, uint node_count, Edge *edges, uint edge_count,
					  uint *node_deleted, uint *edge_deleted) {
	ASSERT(g);

	uint _edge_deleted = 0;
	uint _node_deleted = 0;

	if(node_count) {
		_BulkDeleteNodes(g, nodes, node_count, &_node_deleted, &_edge_deleted);
	}

	if(edge_count) {
		// Filter out explicit edges which were removed by _BulkDeleteNodes.
		if(node_count) {
			for(int i = 0; i < edge_count; i++) {
				Edge *e = edges + i;
				NodeID src = Edge_GetSrcNodeID(e);
				NodeID dest = Edge_GetDestNodeID(e);

				if(!DataBlock_GetItem(g->nodes, src) || !DataBlock_GetItem(g->nodes, dest)) {
					// edge already removed due to node removal
					// replace current edge with last edge
					edges[i] = edges[edge_count - 1];

					// Update indices.
					i--;
					edge_count--;
				}
			}
		}

		// removing duplicates
#define is_edge_lt(a, b) (ENTITY_GET_ID((a)) < ENTITY_GET_ID((b)))
		QSORT(Edge, edges, edge_count, is_edge_lt);

		size_t uniqueIdx = 0;
		for(int i = 0; i < edge_count; i++) {
			// As long as current is the same as follows.
			while(i < edge_count - 1 && ENTITY_GET_ID(edges + i) == ENTITY_GET_ID(edges + i + 1)) i++;

			if(uniqueIdx < i) edges[uniqueIdx] = edges[i];
			uniqueIdx++;
		}

		edge_count = uniqueIdx;
		if(edge_count > 0) _BulkDeleteEdges(g, edges, edge_count);
	}
	_edge_deleted += edge_count;

	if(node_deleted != NULL) *node_deleted = _node_deleted;
	if(edge_deleted != NULL) *edge_deleted = _edge_deleted;
}

DataBlockIterator *Graph_ScanNodes(const Graph *g) {
	ASSERT(g);
	return DataBlock_Scan(g->nodes);
}

DataBlockIterator *Graph_ScanEdges(const Graph *g) {
	ASSERT(g);
	return DataBlock_Scan(g->edges);
}

int Graph_AddLabel
(
	Graph *g
) {
	ASSERT(g != NULL);

	RG_Matrix m;
	GrB_Info info;
	size_t n = Graph_RequiredMatrixDim(g);
	RG_Matrix_new(&m, GrB_BOOL, n, n, false, false);

	array_append(g->labels, m);
	return array_len(g->labels) - 1;
}

int Graph_AddRelationType
(
	Graph *g
) {
	ASSERT(g);

	RG_Matrix m;
	size_t n = Graph_RequiredMatrixDim(g);

	bool maintain_transpose;
	Config_Option_get(Config_MAINTAIN_TRANSPOSE, &maintain_transpose);

	RG_Matrix_new(&m, GrB_UINT64, n, n, true, maintain_transpose);

	array_append(g->relations, m);

	// adding a new relationship type, update the stats structures to support it
	GraphStatistics_IntroduceRelationship(&g->stats);

	int relationID = Graph_RelationTypeCount(g) - 1;
	return relationID;
}

RG_Matrix Graph_GetLabelMatrix
(
	const Graph *g, 
	int label_idx
) {
	ASSERT(g);
	ASSERT(label_idx < array_len(g->labels));

	RG_Matrix m = g->labels[label_idx];
	g->SynchronizeMatrix(g, m);
	return m;
}

RG_Matrix Graph_GetRelationMatrix
(
	const Graph *g,
	int relation_idx, 
	bool transposed
) {
	ASSERT(g);
	ASSERT(relation_idx == GRAPH_NO_RELATION ||
		   relation_idx < Graph_RelationTypeCount(g));

	RG_Matrix m = GrB_NULL;

	if(relation_idx == GRAPH_NO_RELATION) m = g->adjacency_matrix;
	else m = g->relations[relation_idx];
	if(transposed) m = RG_Matrix_getTranspose(m);

	g->SynchronizeMatrix(g, m);

	return m;
}

RG_Matrix Graph_GetAdjacencyMatrix
(
	const Graph *g,
	bool transposed
) {
	return Graph_GetRelationMatrix(g, GRAPH_NO_RELATION, transposed);
}

// returns true if relationship matrix 'r' contains multi-edge entries,
// false otherwise
bool Graph_RelationshipContainsMultiEdge
(
	const Graph *g,
	int r,
	bool transpose
) {
	ASSERT(Graph_RelationTypeCount(g) > r);
	GrB_Index nvals;
	// A relationship matrix contains multi-edge if nvals < number of edges with type r.
	RG_Matrix R = Graph_GetRelationMatrix(g, r, transpose);
	RG_Matrix_nvals(&nvals, R);

	return (Graph_RelationEdgeCount(g, r) > nvals);
}

RG_Matrix Graph_GetZeroMatrix
(
	const Graph *g
) {
	RG_Matrix z = g->_zero_matrix;
	_MatrixResizeToCapacity(g, z);

#if RG_DEBUG
	// make sure zero matrix is indeed empty
	GrB_Index nvals;
	RG_Matrix_nvals(&nvals, z);
	ASSERT(nvals == 0);
#endif

	return z;
}

void Graph_Free(Graph *g) {
	ASSERT(g);
	// free matrices
	Entity *en;
	DataBlockIterator *it;
	RG_Matrix_free(&g->_zero_matrix);
	RG_Matrix_free(&g->adjacency_matrix);

	_Graph_FreeRelationMatrices(g);
	array_free(g->relations);
	GraphStatistics_FreeInternals(&g->stats);

	uint32_t labelCount = array_len(g->labels);
	for(int i = 0; i < labelCount; i++) RG_Matrix_free(&g->labels[i]);
	array_free(g->labels);

	// TODO: disable datablock deleted items array
	// there's no need to keep track after deleted items as the graph
	// is being removed we won't be reusing items
	it = Graph_ScanNodes(g);
	while((en = (Entity *)DataBlockIterator_Next(it, NULL)) != NULL) {
		FreeEntity(en);
	}
	DataBlockIterator_Free(it);

	it = Graph_ScanEdges(g);
	while((en = DataBlockIterator_Next(it, NULL)) != NULL) FreeEntity(en);
	DataBlockIterator_Free(it);

	// free blocks
	DataBlock_Free(g->nodes);
	DataBlock_Free(g->edges);

	int res;
	UNUSED(res);

	if(g->_writelocked) Graph_ReleaseLock(g);
	res = pthread_rwlock_destroy(&g->_rwlock);
	ASSERT(res == 0);

	rm_free(g);
}
