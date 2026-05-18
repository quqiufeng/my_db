#!/usr/bin/env python3
"""
AI Agent Interface for stable-diffusion.cpp

Provides semantic search and structured API access for AI Agents.
"""

import os
import sys
import json

sys.path.insert(0, '/home/dministrator/my_db')
from mydb.cache import open_cache

class StableDiffusionAgent:
    def __init__(self, cache_dir='./ai_code_memory'):
        self.cache = open_cache(cache_dir)
        self.namespace = '/code/local/stable-diffusion.cpp'
    
    def query_api(self, query: str):
        """Query API by function name or description"""
        query = query.lower()
        results = []
        
        # Search symbols
        for key in self.cache.keys():
            if key.startswith(f'{self.namespace}/symbols/'):
                name = key.split('/')[-1]
                if query in name.lower():
                    data = json.loads(self.cache.get(key))
                    results.append({
                        'type': 'symbol',
                        'name': name,
                        'locations': data
                    })
        
        # Search chunks
        for key in self.cache.keys():
            if key.startswith(f'{self.namespace}/chunks/'):
                name = key.split('/')[-1]
                if query in name.lower():
                    data = json.loads(self.cache.get(key))
                    results.append({
                        'type': 'implementation',
                        'name': name,
                        'file': data.get('file'),
                        'line': data.get('line_start'),
                        'signature': data.get('signature')
                    })
        
        return results
    
    def get_function_doc(self, func_name: str):
        """Get documentation for a specific function"""
        key = f'{self.namespace}/chunks/include/stable-diffusion.h/{func_name}'
        data = self.cache.get(key)
        if data:
            return json.loads(data)
        return None
    
    def list_capabilities(self):
        """List all image generation capabilities"""
        meta = self.cache.get(f'{self.namespace}/_meta/agent_summary')
        if meta:
            return json.loads(meta).get('capabilities', [])
        return []
    
    def get_workflow(self, task: str):
        """Get workflow for a specific task"""
        workflows = {
            'text2image': {
                'description': 'Generate image from text',
                'steps': [
                    'sd_ctx_params_init(&params)',
                    'params.model_path = "model.safetensors"',
                    'sd_ctx_t* ctx = new_sd_ctx(&params)',
                    'sd_img_gen_params_init(&gen)',
                    'gen.prompt = "your prompt"',
                    'gen.width = 512; gen.height = 512',
                    'sd_image_t* img = generate_image(ctx, &gen)',
                    'free_sd_ctx(ctx)'
                ],
                'key_params': ['prompt', 'width', 'height', 'sample_steps', 'seed']
            },
            'image2image': {
                'description': 'Edit existing image',
                'steps': [
                    'Load init_image from file',
                    'sd_img_gen_params_init(&gen)',
                    'gen.init_image = loaded_image',
                    'gen.strength = 0.75',  # 0.0=unchanged, 1.0=full redraw
                    'gen.prompt = "edit prompt"',
                    'generate_image(ctx, &gen)'
                ],
                'key_params': ['init_image', 'strength', 'prompt']
            },
            'inpainting': {
                'description': 'Fill masked regions',
                'steps': [
                    'Load init_image and mask_image',
                    'sd_img_gen_params_init(&gen)',
                    'gen.init_image = image',
                    'gen.mask_image = mask',  # White=inpaint, Black=keep
                    'gen.prompt = "fill description"',
                    'generate_image(ctx, &gen)'
                ],
                'key_params': ['init_image', 'mask_image', 'prompt']
            },
            'upscaling': {
                'description': 'Upscale image 2x-4x',
                'steps': [
                    'upscaler_ctx_t* up = new_upscaler_ctx("esrgan.pth", false, false, 4, 128)',
                    'sd_image_t result = upscale(up, input_image, 4)',
                    'free_upscaler_ctx(up)'
                ],
                'key_params': ['upscale_factor', 'model_path']
            },
            'controlnet': {
                'description': 'Control generation with edge map',
                'steps': [
                    'preprocess_canny(input_image, 0.08, 0.03, 0.1, 1.0, false)',
                    'sd_img_gen_params_init(&gen)',
                    'gen.control_image = canny_result',
                    'gen.control_strength = 0.9',
                    'gen.prompt = "prompt"',
                    'generate_image(ctx, &gen)'
                ],
                'key_params': ['control_image', 'control_strength']
            }
        }
        return workflows.get(task.lower(), {'error': 'Unknown task'})
    
    def get_parameter_guide(self):
        """Get guide for important parameters"""
        return {
            'width/height': {
                'description': 'Image dimensions',
                'constraints': 'Must be multiples of 64',
                'common': [512, 768, 1024]
            },
            'sample_steps': {
                'description': 'Denoising steps',
                'range': '1-150',
                'recommended': '20-50 (higher= better quality, slower)'
            },
            'sample_method': {
                'description': 'Sampling algorithm',
                'options': ['EULER_A', 'DPMpp2M', 'LCM', 'DDIM'],
                'recommended': 'EULER_A for quality, LCM for speed'
            },
            'cfg_scale': {
                'description': 'Classifier-free guidance scale',
                'range': '1.0-30.0',
                'recommended': '7.0-12.0'
            },
            'seed': {
                'description': 'Random seed for reproducibility',
                'special': '-1 for random, >=0 for fixed'
            },
            'strength': {
                'description': 'Img2img strength',
                'range': '0.0-1.0',
                'meaning': '0.0=unchanged, 1.0=complete redraw'
            }
        }
    
    def search_code(self, query: str):
        """Semantic search for code related to query"""
        # Simple keyword search (can be enhanced with vectors)
        results = []
        query_lower = query.lower()
        
        for key in self.cache.keys():
            if not key.startswith(f'{self.namespace}/chunks/'):
                continue
            try:
                data = json.loads(self.cache.get(key))
                text = f"{data.get('signature', '')} {data.get('docstring', '')}"
                if query_lower in text.lower():
                    results.append({
                        'name': key.split('/')[-1],
                        'file': data.get('file'),
                        'line': data.get('line_start'),
                        'signature': data.get('signature')
                    })
            except:
                pass
        
        return results[:20]  # Limit results


def main():
    agent = StableDiffusionAgent()
    
    print("🤖 Stable Diffusion C++ - AI Agent Interface")
    print("="*60)
    
    # Demo queries
    demos = [
        ('api', 'generate_image'),
        ('workflow', 'text2image'),
        ('workflow', 'controlnet'),
        ('search', 'flash attention'),
        ('params', None)
    ]
    
    for demo_type, demo_query in demos:
        print(f"\n📌 Demo: {demo_type} = {demo_query}")
        
        if demo_type == 'api':
            results = agent.query_api(demo_query)
            for r in results[:3]:
                print(f"  {r['name']} ({r['type']})")
                if r['type'] == 'implementation':
                    print(f"    File: {r['file']}:{r['line']}")
                    print(f"    Signature: {r['signature']}")
        
        elif demo_type == 'workflow':
            wf = agent.get_workflow(demo_query)
            print(f"  Description: {wf.get('description')}")
            print(f"  Key params: {', '.join(wf.get('key_params', []))}")
            print(f"  Steps:")
            for step in wf.get('steps', [])[:5]:
                print(f"    → {step}")
        
        elif demo_type == 'search':
            results = agent.search_code(demo_query)
            print(f"  Found {len(results)} matches")
            for r in results[:5]:
                print(f"    {r['name']} in {r['file']}:{r['line']}")
        
        elif demo_type == 'params':
            params = agent.get_parameter_guide()
            for name, info in params.items():
                print(f"  {name}: {info['description']}")
                if 'recommended' in info:
                    print(f"    Recommended: {info['recommended']}")

if __name__ == '__main__':
    main()
