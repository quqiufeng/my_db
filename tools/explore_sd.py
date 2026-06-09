#!/usr/bin/env python3
"""
Stable Diffusion C++ Explorer - AI Agent Interface

This script explores the stable-diffusion.cpp project structure and provides
an easy-to-use interface for AI Agents to understand the image generation APIs.
"""

import os
import sys
import json
from pathlib import Path

sys.path.insert(0, '/home/dministrator/my_db')
from mydb.cache import open_cache

SD_DIR = '/opt/stable-diffusion.cpp'
cache = open_cache('/memory')

def explore_structure():
    """Explore project structure"""
    print("="*70)
    print("STABLE-DIFFUSION.CPP PROJECT EXPLORATION")
    print("="*70)
    
    # Count files by type
    files = {
        'C/C++ Source': [],
        'Headers': [],
        'CUDA': [],
        'Python': [],
        'Build/Config': [],
        'Other': []
    }
    
    for root, dirs, filenames in os.walk(SD_DIR):
        # Skip build and thirdparty dirs
        dirs[:] = [d for d in dirs if d not in ['build', 'thirdparty', '.git', 'bin']]
        
        for f in filenames:
            path = os.path.join(root, f)
            rel = os.path.relpath(path, SD_DIR)
            if f.endswith(('.c', '.cpp', '.cc')):
                files['C/C++ Source'].append(rel)
            elif f.endswith(('.h', '.hpp')):
                files['Headers'].append(rel)
            elif f.endswith('.cu'):
                files['CUDA'].append(rel)
            elif f.endswith('.py'):
                files['Python'].append(rel)
            elif f.endswith(('.txt', '.cmake', 'CMakeLists.txt', 'Makefile')):
                files['Build/Config'].append(rel)
            else:
                files['Other'].append(rel)
    
    print("\n📁 FILE STRUCTURE:")
    for ftype, flist in files.items():
        if flist:
            print(f"  {ftype}: {len(flist)} files")
    
    return files

def analyze_api():
    """Analyze public API from stable-diffusion.h"""
    print("\n" + "="*70)
    print("🎯 PUBLIC API ANALYSIS")
    print("="*70)
    
    api_categories = {
        'Context Management': [],
        'Image Generation': [],
        'Video Generation': [],
        'Image Processing': [],
        'Model Utilities': [],
        'Type Converters': [],
    }
    
    # Read from cache
    namespace = '/code/local/stable-diffusion.cpp'
    
    # Get all functions from header
    header_funcs = []
    for key in cache.keys():
        if key.startswith(f'{namespace}/chunks/include/stable-diffusion.h/'):
            name = key.split('/')[-1]
            header_funcs.append(name)
    
    print(f"\n📌 Found {len(header_funcs)} API functions in stable-diffusion.h")
    
    # Key functions
    key_apis = [
        ('new_sd_ctx', 'Create SD context with model parameters'),
        ('free_sd_ctx', 'Release SD context'),
        ('generate_image', 'Generate image from text prompt'),
        ('generate_video', 'Generate video frames'),
        ('upscale', 'Upscale image using ESRGAN'),
        ('preprocess_canny', 'Canny edge detection preprocessing'),
        ('convert', 'Convert model format'),
        ('sd_set_log_callback', 'Set logging callback'),
        ('sd_set_progress_callback', 'Set progress callback'),
        ('sd_set_preview_callback', 'Set preview callback'),
        ('sd_ctx_params_init', 'Initialize context parameters'),
        ('sd_img_gen_params_init', 'Initialize image generation parameters'),
        ('sd_sample_params_init', 'Initialize sampling parameters'),
    ]
    
    print("\n🔑 KEY API FUNCTIONS:")
    for func, desc in key_apis:
        print(f"  • {func:30s} - {desc}")
    
    return key_apis

def analyze_workflow():
    """Analyze typical image generation workflow"""
    print("\n" + "="*70)
    print("🔄 IMAGE GENERATION WORKFLOW")
    print("="*70)
    
    workflow = """
1. INITIALIZE CONTEXT
   sd_ctx_params_t params;
   sd_ctx_params_init(&params);
   params.model_path = "model.safetensors";
   params.n_threads = 4;
   params.wtype = SD_TYPE_F16;  // or SD_TYPE_Q4_0 for quantized
   
   sd_ctx_t* ctx = new_sd_ctx(&params);

2. SETUP GENERATION PARAMETERS
   sd_img_gen_params_t gen_params;
   sd_img_gen_params_init(&gen_params);
   gen_params.prompt = "a beautiful sunset over mountains";
   gen_params.negative_prompt = "blurry, low quality";
   gen_params.width = 512;
   gen_params.height = 512;
   gen_params.sample_params.sample_method = EULER_A_SAMPLE_METHOD;
   gen_params.sample_params.sample_steps = 20;
   gen_params.seed = 42;
   gen_params.batch_count = 1;

3. GENERATE IMAGE
   sd_image_t* images = generate_image(ctx, &gen_params);
   // images[0].data contains RGB bytes
   // images[0].width, images[0].height, images[0].channel

4. CLEANUP
   free_sd_ctx(ctx);
"""
    print(workflow)

def analyze_image_processing():
    """Analyze image processing capabilities"""
    print("\n" + "="*70)
    print("🖼️ IMAGE PROCESSING CAPABILITIES")
    print("="*70)
    
    capabilities = [
        ("Text-to-Image", "Generate image from text prompt using diffusion model"),
        ("Image-to-Image", "Modify existing image with prompt and strength parameter"),
        ("Inpainting", "Fill masked regions using init_image + mask_image"),
        ("Upscaling", "2x-4x upscaling with ESRGAN models"),
        ("ControlNet", "Control generation with control_image + control_strength"),
        ("LoRA", "Apply LoRA weights for style/character control"),
        ("PhotoMaker", "Generate photos from ID images"),
        ("HiRes Fix", "Two-pass generation for higher resolution"),
        ("Tiling", "Process large images in tiles to save VRAM"),
        ("Canny Preprocessing", "Edge detection for ControlNet"),
        ("Video Generation", "Generate video from prompts (Wan/SkyReels)"),
        ("FreeU", "Enhance detail without extra computation"),
        ("SAG", "Self-attention guidance for better quality"),
        ("Dynamic CFG", "Dynamic classifier-free guidance scaling"),
    ]
    
    for cap, desc in capabilities:
        print(f"  • {cap:25s} - {desc}")

def analyze_model_formats():
    """Analyze supported model formats"""
    print("\n" + "="*70)
    print("📊 SUPPORTED MODEL FORMATS")
    print("="*70)
    
    formats = {
        'Checkpoint': ['.safetensors', '.ckpt'],
        'Diffusion Model': ['.safetensors', '.gguf'],
        'VAE': ['.safetensors', '.pt', '.bin'],
        'Text Encoder': ['.safetensors', '.gguf'],
        'LoRA': ['.safetensors'],
        'ControlNet': ['.safetensors', '.pth'],
        'ESRGAN': ['.pth'],
        'Embeddings': ['.pt', '.safetensors'],
    }
    
    for fmt, exts in formats.items():
        print(f"  • {fmt:20s} - {', '.join(exts)}")
    
    print("\n💾 QUANTIZATION TYPES:")
    types = [
        ("SD_TYPE_F32", "Full precision (4 bytes)"),
        ("SD_TYPE_F16", "Half precision (2 bytes)"),
        ("SD_TYPE_Q4_0", "4-bit quantized (0.5 bytes)"),
        ("SD_TYPE_Q5_0", "5-bit quantized"),
        ("SD_TYPE_Q8_0", "8-bit quantized (1 byte)"),
        ("SD_TYPE_Q4_K", "Q4_K Merged"),
        ("SD_TYPE_Q5_K", "Q5_K Merged"),
        ("SD_TYPE_Q6_K", "Q6_K Merged"),
        ("SD_TYPE_IQ2_XXS", "2-bit super-quantized"),
        ("SD_TYPE_IQ4_NL", "4-bit non-linear"),
        ("SD_TYPE_BF16", "BFloat16"),
    ]
    
    for t, desc in types:
        print(f"  • {t:20s} - {desc}")

def analyze_performance_features():
    """Analyze performance optimization features"""
    print("\n" + "="*70)
    print("⚡ PERFORMANCE FEATURES")
    print("="*70)
    
    features = [
        ("Flash Attention", "Memory-efficient attention (reduce VRAM by ~50%)"),
        ("TAE Preview", "Tiny AutoEncoder for fast preview during generation"),
        ("VAE Tiling", "Process large images in tiles to avoid OOM"),
        ("CPU Offloading", "Offload params to CPU when not in use"),
        ("MMAP Loading", "Memory-map model files instead of loading"),
        ("Cache Modes", "EasyCache, UCache, DBCache, TaylorSeer, Spectrum"),
        ("Multi-threading", "Configurable thread count for CPU ops"),
        ("CUDA/GPU", "CUDA acceleration for NVIDIA GPUs"),
        ("Batch Processing", "Generate multiple images in one call"),
    ]
    
    for feat, desc in features:
        print(f"  • {feat:25s} - {desc}")

def generate_agent_summary():
    """Generate summary for AI Agent"""
    print("\n" + "="*70)
    print("🤖 AI AGENT QUICK REFERENCE")
    print("="*70)
    
    summary = {
        "project": "stable-diffusion.cpp",
        "type": "C++ inference engine for Stable Diffusion",
        "capabilities": [
            "Text-to-image generation",
            "Image-to-image editing",
            "Inpainting/outpainting",
            "Video generation",
            "Image upscaling",
            "ControlNet guidance",
            "LoRA fine-tuning application"
        ],
        "key_api": {
            "context": "sd_ctx_params_init → new_sd_ctx → free_sd_ctx",
            "generate": "sd_img_gen_params_init → generate_image",
            "upscale": "new_upscaler_ctx → upscale → free_upscaler_ctx",
            "callbacks": "sd_set_progress_callback, sd_set_preview_callback"
        },
        "memory_optimization": [
            "Use quantized models (Q4_0, Q5_0)",
            "Enable flash_attention",
            "Use VAE tiling for large images",
            "Enable CPU offloading",
            "Use mmap for model loading"
        ],
        "important_params": {
            "width/height": "Image dimensions (multiples of 64)",
            "sample_steps": "Quality vs speed tradeoff (20-50)",
            "cfg_scale": "Prompt adherence (7-12 typical)",
            "seed": "Reproducibility (-1 for random)",
            "strength": "For img2img (0.0-1.0)"
        }
    }
    
    print(json.dumps(summary, indent=2))
    
    # Save to cache for agent use
    namespace = '/code/local/stable-diffusion.cpp'
    cache.set_json(f'{namespace}/_meta/agent_summary', summary, 0)
    cache.sync()
    print("\n✅ Summary saved to cache")

if __name__ == '__main__':
    explore_structure()
    analyze_api()
    analyze_workflow()
    analyze_image_processing()
    analyze_model_formats()
    analyze_performance_features()
    generate_agent_summary()
