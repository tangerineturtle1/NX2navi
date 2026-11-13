// Level-of-detail settings for Ocarina: the host functions of the set lod's sites and the start (lod.c).
#pragma once
#include "cpu.h"

void lod_init(void);                 // boot: the patch page's words, after rt_patches_init
int lod_available(void);             // the generated C has the set lod: the settings act
int lod_tree_shadow_split(void);     // the low trees' shadow distance is changed: the tree shader takes it
uint32_t lod_foliage_capacity(void); // the factor on the foliage lists' capacities; 1: the game's
uint32_t lod_foliage_heap_kib(void); // what the Foliage heap grows by for those capacities; 0: nothing
void lod_frame(void);                // once per presented frame: the live settings
void lod_param_host(Cpu *c);         // patch page +0xf20
void lod_plant_host(Cpu *c);         // +0xf50
void lod_model_max_host(Cpu *c);     // +0xf40
void lod_model_start_host(Cpu *c);   // +0xf30
void lod_foliage_host(Cpu *c);       // +0xf10
