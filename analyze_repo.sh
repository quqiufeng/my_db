#!/bin/bash
# analyze_repo.sh — Master orchestration: full pipeline from source to memory
#
# Usage:
#     ./analyze_repo.sh <source> [namespace] [options]
#
# Arguments:
#     source     - GitHub URL or local path
#     namespace  - Target namespace (default: auto-detect from source)
#
# Options:
#     --skip-vectors        Skip vector generation (faster, no semantic search)
#     --skip-callgraph      Skip call graph analysis
#     --skip-dataflow       Skip dataflow analysis
#     --cache-dir <dir>     KV Cache directory (default: ./ai_code_memory)
#     --jobs <n>            Number of parallel jobs (default: auto)
#     --name <name>        Project name for cache directory
#
# Examples:
#     # Analyze GitHub repo
#     ./analyze_repo.sh https://github.com/redis/redis /code/redis
#
#     # Analyze local project
#     ./analyze_repo.sh /home/user/myproject /code/myproject
#
#     # Quick analysis (no vectors)
#     ./analyze_repo.sh https://github.com/sqlite/sqlite /code/sqlite --skip-vectors
#
# Pipeline:
#     1. Clone/download source
#     2. Index code (ctags + AST)
#     3. Generate vectors (TensorRT GPU)
#     4. Analyze call graph
#     5. Analyze dataflow
#     6. Import everything into KV Cache

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log() {
    echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

# Parse arguments
SOURCE=""
NAMESPACE=""
SKIP_VECTORS=false
SKIP_CALLGRAPH=false
SKIP_DATAFLOW=false
CACHE_DIR="./ai_code_memory"
JOBS=""
PROJECT_NAME=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-vectors)
            SKIP_VECTORS=true
            shift
            ;;
        --skip-callgraph)
            SKIP_CALLGRAPH=true
            shift
            ;;
        --skip-dataflow)
            SKIP_DATAFLOW=true
            shift
            ;;
        --cache-dir)
            CACHE_DIR="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --name)
            PROJECT_NAME="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 <source> [namespace] [options]"
            echo ""
            echo "Arguments:"
            echo "  source     GitHub URL or local path"
            echo "  namespace  Target namespace (default: auto-detect)"
            echo ""
            echo "Options:"
            echo "  --skip-vectors     Skip vector generation"
            echo "  --skip-callgraph   Skip call graph analysis"
            echo "  --skip-dataflow    Skip dataflow analysis"
            echo "  --cache-dir <dir>  KV Cache directory"
            echo "  --jobs <n>         Parallel jobs"
            echo "  --name <name>      Project name"
            exit 0
            ;;
        -*)
            error "Unknown option: $1"
            exit 1
            ;;
        *)
            if [[ -z "$SOURCE" ]]; then
                SOURCE="$1"
            elif [[ -z "$NAMESPACE" ]]; then
                NAMESPACE="$1"
            else
                error "Too many arguments"
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "$SOURCE" ]]; then
    error "No source specified"
    echo "Usage: $0 <source> [namespace] [options]"
    exit 1
fi

# Auto-detect namespace
if [[ -z "$NAMESPACE" ]]; then
    if [[ "$SOURCE" =~ ^https://github.com/([^/]+)/([^/]+) ]]; then
        OWNER="${BASH_REMATCH[1]}"
        REPO="${BASH_REMATCH[2]}"
        NAMESPACE="/code/$OWNER/$REPO"
    else
        REPO=$(basename "$SOURCE")
        NAMESPACE="/code/local/$REPO"
    fi
fi

# Auto-detect project name
if [[ -z "$PROJECT_NAME" ]]; then
    PROJECT_NAME=$(basename "$NAMESPACE")
fi

# Auto-detect jobs
if [[ -z "$JOBS" ]]; then
    JOBS=$(nproc)
fi

# Determine analysis directory
ANALYSIS_DIR="./${PROJECT_NAME}_cache"

log "================================================================"
log "Analyzing repository: $SOURCE"
log "Namespace: $NAMESPACE"
log "Analysis directory: $ANALYSIS_DIR"
log "Cache directory: $CACHE_DIR"
log "Jobs: $JOBS"
log "Skip vectors: $SKIP_VECTORS"
log "Skip callgraph: $SKIP_CALLGRAPH"
log "Skip dataflow: $SKIP_DATAFLOW"
log "================================================================"

# Step 1: Clone or use local source
SOURCE_DIR=""
CLEANUP_DIR=""

if [[ "$SOURCE" =~ ^https?:// ]]; then
    log "Step 1: Cloning repository..."
    SOURCE_DIR=$(mktemp -d)
    CLEANUP_DIR="$SOURCE_DIR"
    
    if ! git clone --depth 1 "$SOURCE" "$SOURCE_DIR" 2>&1; then
        error "Failed to clone repository"
        rm -rf "$CLEANUP_DIR"
        exit 1
    fi
    
    success "Cloned to $SOURCE_DIR"
else
    if [[ ! -d "$SOURCE" ]]; then
        error "Source directory does not exist: $SOURCE"
        exit 1
    fi
    SOURCE_DIR="$SOURCE"
    success "Using local directory: $SOURCE_DIR"
fi

# Step 2: Index code
log "Step 2: Indexing code..."
if [[ -f "$SCRIPT_DIR/tools/code_indexer" ]]; then
    INDEXER="$SCRIPT_DIR/tools/code_indexer"
elif [[ -f "$SCRIPT_DIR/code_indexer" ]]; then
    INDEXER="$SCRIPT_DIR/code_indexer"
else
    error "code_indexer not found"
    exit 1
fi

if ! "$INDEXER" "$SOURCE_DIR" "$ANALYSIS_DIR" "$JOBS" 2>&1; then
    warn "Indexer returned non-zero, continuing..."
fi

if [[ ! -f "$ANALYSIS_DIR/chunks_meta.jsonl" ]]; then
    error "Indexing failed: chunks_meta.jsonl not created"
    rm -rf "$CLEANUP_DIR"
    exit 1
fi

CHUNK_COUNT=$(wc -l < "$ANALYSIS_DIR/chunks_meta.jsonl")
success "Indexed $CHUNK_COUNT chunks"

# Step 3: Generate vectors (optional)
if [[ "$SKIP_VECTORS" == false ]]; then
    log "Step 3: Generating vectors..."
    
    if [[ -f "$SCRIPT_DIR/tools/batch_embedder" ]]; then
        EMBEDDER="$SCRIPT_DIR/tools/batch_embedder"
    elif [[ -f "$SCRIPT_DIR/batch_embedder" ]]; then
        EMBEDDER="$SCRIPT_DIR/batch_embedder"
    else
        warn "batch_embedder not found, skipping vectors"
        SKIP_VECTORS=true
    fi
    
    if [[ "$SKIP_VECTORS" == false ]]; then
        if ! "$EMBEDDER" "$ANALYSIS_DIR" --model jina --name "$PROJECT_NAME" 2>&1; then
            warn "Vector generation failed, continuing..."
            SKIP_VECTORS=true
        else
            success "Vectors generated"
        fi
    fi
else
    log "Step 3: Skipping vector generation"
fi

# Step 4: Analyze call graph (optional)
if [[ "$SKIP_CALLGRAPH" == false ]]; then
    log "Step 4: Analyzing call graph..."
    
    if [[ -f "$SCRIPT_DIR/tools/call_graph" ]]; then
        CALLGRAPH="$SCRIPT_DIR/tools/call_graph"
    elif [[ -f "$SCRIPT_DIR/call_graph" ]]; then
        CALLGRAPH="$SCRIPT_DIR/call_graph"
    else
        warn "call_graph tool not found, skipping"
        SKIP_CALLGRAPH=true
    fi
    
    if [[ "$SKIP_CALLGRAPH" == false ]]; then
        if ! "$CALLGRAPH" "$ANALYSIS_DIR" 2>&1; then
            warn "Call graph analysis failed, continuing..."
        else
            if [[ -f "$ANALYSIS_DIR/call_graph.json" ]]; then
                EDGE_COUNT=$(grep -c '"calls"' "$ANALYSIS_DIR/call_graph.json" 2>/dev/null || echo "0")
                success "Call graph analyzed ($EDGE_COUNT edges)"
            fi
        fi
    fi
else
    log "Step 4: Skipping call graph analysis"
fi

# Step 5: Analyze dataflow (optional)
if [[ "$SKIP_DATAFLOW" == false ]]; then
    log "Step 5: Analyzing dataflow..."
    
    if [[ -f "$SCRIPT_DIR/tools/dataflow" ]]; then
        DATAFLOW="$SCRIPT_DIR/tools/dataflow"
    elif [[ -f "$SCRIPT_DIR/dataflow" ]]; then
        DATAFLOW="$SCRIPT_DIR/dataflow"
    else
        warn "dataflow tool not found, skipping"
        SKIP_DATAFLOW=true
    fi
    
    if [[ "$SKIP_DATAFLOW" == false ]]; then
        if ! "$DATAFLOW" analyze "$ANALYSIS_DIR" 2>&1; then
            warn "Dataflow analysis failed, continuing..."
        else
            success "Dataflow analyzed"
        fi
    fi
else
    log "Step 5: Skipping dataflow analysis"
fi

# Step 6: Import to KV Cache
log "Step 6: Importing to KV Cache..."

if [[ -f "$SCRIPT_DIR/code_to_memory.sh" ]]; then
    IMPORTER="$SCRIPT_DIR/code_to_memory.sh"
else
    error "code_to_memory.sh not found"
    rm -rf "$CLEANUP_DIR"
    exit 1
fi

if ! $IMPORTER "$ANALYSIS_DIR" "$NAMESPACE" --cache-dir "$CACHE_DIR" 2>&1; then
    error "Failed to import to KV Cache"
    rm -rf "$CLEANUP_DIR"
    exit 1
fi

success "Imported to KV Cache"

# Cleanup
if [[ -n "$CLEANUP_DIR" ]]; then
    log "Cleaning up temporary files..."
    rm -rf "$CLEANUP_DIR"
fi

# Summary
log "================================================================"
success "Analysis complete!"
log "Project: $PROJECT_NAME"
log "Namespace: $NAMESPACE"
log "Cache: $CACHE_DIR"
log "Chunks: $CHUNK_COUNT"
log "Vectors: $([ "$SKIP_VECTORS" == true ] && echo "skipped" || echo "generated")"
log "Call graph: $([ "$SKIP_CALLGRAPH" == true ] && echo "skipped" || echo "analyzed")"
log "Dataflow: $([ "$SKIP_DATAFLOW" == true ] && echo "skipped" || echo "analyzed")"
log ""
log "Query examples:"
log "  ./agent_query.sh <symbol> --repo $NAMESPACE --type context"
log "  ./agent_query.sh \"search query\" --repo $NAMESPACE --type search"
log "================================================================"
