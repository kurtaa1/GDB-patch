/* Native-dependent code for GNU/Linux SPARC.
   Copyright (C) 2005-2016 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "regcache.h"

#include <sys/procfs.h>
#include "gregset.h"

#include "sparc-tdep.h"
#include "sparc-nat.h"
#include "inferior.h"
#include "target.h"
#include "linux-nat.h"

#include "break-common.h"
#include "nat/gdb_ptrace.h"
#include "nat/linux-ptrace.h"

#define PTRACE_SETHBREGS 27
#define PTRACE_GETHBREGS 28
#define REMOVE_BREAKPOINT 8
#define INSERT_BREAKPOINT(TYPE) TYPE
#define CHANGE_MASK(SLOT) 4 + SLOT
				 
void
supply_gregset (struct regcache *regcache, const prgregset_t *gregs)
{
  sparc32_supply_gregset (sparc_gregmap, regcache, -1, gregs);
}

void
supply_fpregset (struct regcache *regcache, const prfpregset_t *fpregs)
{
  sparc32_supply_fpregset (sparc_fpregmap, regcache, -1, fpregs);
}

void
fill_gregset (const struct regcache *regcache, prgregset_t *gregs, int regnum)
{
  sparc32_collect_gregset (sparc_gregmap, regcache, regnum, gregs);
}

void
fill_fpregset (const struct regcache *regcache,
	       prfpregset_t *fpregs, int regnum)
{
  sparc32_collect_fpregset (sparc_fpregmap, regcache, regnum, fpregs);
}


struct sparc_linux_hw_breakpoint{
	unsigned int address;
	unsigned int mask;
	enum target_hw_bp_type type;
	int enabled;
	int hw_slot;
};

#define SPARC_MAX_HW_BPS 4
/*Stores information about the hardware breakpoints associated with a certain inferior
 * Contains inserted breakpoints only*/
typedef struct sparc_linux_inferior_bps{
	ptid_t id;
	struct sparc_linux_hw_breakpoint *bps[SPARC_MAX_HW_BPS];
	int num;
} *inf_bp_list;

DEF_VEC_P(inf_bp_list);
VEC(inf_bp_list) *inf_list = NULL;

static int hw_breakpoint_equal(struct sparc_linux_hw_breakpoint bp1, struct sparc_linux_hw_breakpoint bp2){
	return bp1.address == bp2.address && bp1.type == bp2.type;
}

/*Returns the breakpoint that equals bp in the inferior list. If none exist, return null*/
static struct sparc_linux_hw_breakpoint * get_breakpoint_in_inf_list(struct sparc_linux_inferior_bps * list, 
											struct sparc_linux_hw_breakpoint bp){
	int i;
	for(i=0; i<SPARC_MAX_HW_BPS; i++){
		if(list->bps[i] != NULL){
			if(hw_breakpoint_equal(*list->bps[i], bp))
				return list->bps[i];
		}
	}
	return NULL;
}
/*Insert a breakpoint structure in an inferior's breakpoint list*/
static int insert_breakpoint_in_inf_list(struct sparc_linux_inferior_bps * list, struct sparc_linux_hw_breakpoint *bp){
	int i;
	for(i = 0; SPARC_MAX_HW_BPS; i++){
		if(list->bps[i] == NULL){
			list->bps[i] = bp;
			return 0;
		}
	}
	return -1;
}

/*Remove a breakpoint structure from an inferior's breakpoint list*/
static int remove_breakpoint_in_inf_list(struct sparc_linux_inferior_bps * list, struct sparc_linux_hw_breakpoint *bp){
	int i;
	for(i = 0; SPARC_MAX_HW_BPS; i++){
		if(list->bps[i] == bp){
			list->bps[i] = NULL;
			return 0;
		}
	}
	return -1;
}

/*Align an address to fit the SPARC architecture*/
static CORE_ADDR sparc_place_addr(CORE_ADDR addr){
	int int_addr = ((unsigned int) addr);
	//clear two LSBs
	return (CORE_ADDR)(int_addr & 0xfffffffc);
}

/*Return sparc_linux_inferior_bps associated with the process id. If none exist create a new one*/
static struct sparc_linux_inferior_bps*
sparc_linux_get_inferior_bps (ptid_t id){
  int i;
  struct sparc_linux_inferior_bps *inf_bps;
  
  for (i = 0; VEC_iterate(inf_bp_list, inf_list, i, inf_bps); i++){
	  if(ptid_equal(inf_bps->id, id))
		return inf_bps;
  }
  //if none exists create a new bp list for the inferior
  inf_bps = (struct sparc_linux_inferior_bps *) xcalloc (1,sizeof (struct sparc_linux_inferior_bps));
  inf_bps->id = id; 
  inf_bps->num = 0;

  VEC_safe_push(inf_bp_list, inf_list , inf_bps);
  return inf_bps;
}
					 
/*Returns 1 if there is still room for new hardware breakpoints in the inferior.*/
static int sparc_linux_can_use_hw_breakpoint (struct target_ops *t,
			enum bptype type, int cnt, int othertype)
{
	if(cnt > SPARC_MAX_HW_BPS)
		return -1;
	return 1;			 
}

/*Inserts a hardware breakpoint/watchpoint with a certain address, type and mask
 * This function is used by the target operations for insertion of all types*/
static int sparc_linux_insert_hw_breakpoint_1 (CORE_ADDR address, 
					  enum target_hw_bp_type type, unsigned int mask)
{
  struct sparc_linux_hw_breakpoint *bp;
  struct sparc_linux_inferior_bps *inf_bps;	
  int inf_pid;
  int r;
  int r2;
  int i;
  
  //Get breakpoint list for this inferior
  inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);
  
  bp = (struct sparc_linux_hw_breakpoint *) xcalloc (1, sizeof (struct sparc_linux_hw_breakpoint));
  bp->address = (unsigned int)address;
  bp->type = type;
  bp->enabled = 1;
  inf_pid = ptid_get_pid(inferior_ptid);
  
  /*Check if breakpoint already exists
   *This disallows duplicate breakpoints to exists*/
  if(get_breakpoint_in_inf_list(inf_bps, *bp) != NULL){
    printf("could not insert: breakpoint already exists\n");
	goto dealloc;
  }
  
  if(insert_breakpoint_in_inf_list(inf_bps, bp) != 0){
	  printf("could not insert: breakpoint list full\n");
	  goto dealloc;
  }
  //Sending bp->type as data will INSERT a breakpoint of the given type
  r = ptrace(PTRACE_SETHBREGS, inf_pid, bp->address, INSERT_BREAKPOINT(bp->type));
  bp->hw_slot = r;
  
  if(r < 0)
	goto dealloc;
	
  //If a mask was specified, edit the mask of the breakpoint
  if(mask != 0){
	r2 = ptrace(PTRACE_SETHBREGS, inf_pid, bp->mask, CHANGE_MASK(r));
	bp->mask = mask;
  }
	
  return 0;
  
  dealloc: xfree(bp);
  return -1;
}

/*Removes a hardware breakpoint/watchpoint with a certain address, type and mask
 * This function is used by the target operations for insertion of all types*/
static int sparc_linux_remove_hw_breakpoint_1(struct sparc_linux_hw_breakpoint comp_bp)
{
	struct sparc_linux_hw_breakpoint *bp;
	struct sparc_linux_inferior_bps *inf_bps;
	int r = 0;
	int inf_pid;
	
	//get bps list for this inferior
	inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);	
	inf_pid = ptid_get_pid(inferior_ptid);
	bp = get_breakpoint_in_inf_list(inf_bps, comp_bp);
	if(bp == NULL){
		printf("could not remove: breakpoint not found\n");
		return -1;
	}
	//Sending 8 as data will REMOVE in the breakpoint slot provided by bp->hw_slot
	r = ptrace(PTRACE_SETHBREGS, inf_pid, bp->hw_slot , REMOVE_BREAKPOINT);
	remove_breakpoint_in_inf_list(inf_bps, bp);
	if(r < 0)
		return -1;
	xfree(bp);	
	return 0;
}

/*Target operation for removing a hardware breakpoint*/
static int sparc_linux_remove_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
	struct sparc_linux_hw_breakpoint comp_bp; 			//comparator breakpoint
	comp_bp.address = (unsigned int) info->placed_address;
	comp_bp.type = hw_execute;
	return sparc_linux_remove_hw_breakpoint_1(comp_bp);
}

/*Target operation for inserting a hardware breakpoint*/
static int sparc_linux_insert_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
	CORE_ADDR address = sparc_place_addr(info->reqstd_address);
    info->placed_address = address;
	return sparc_linux_insert_hw_breakpoint_1(address, hw_execute, 0);
}

/* Set/clear a hardware watchpoint starting at ADDR, for LEN bytes.
   TYPE is 0 for write, 1 for read, and 2 for read/write accesses.
   COND is the expression for its condition, or NULL if there's none.
   Returns 0 for success, 1 if the watchpoint type is not supported,
   -1 for failure.  */
static int sparc_linux_remove_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len,
				 enum target_hw_bp_type type, struct expression *cond)
{
  struct sparc_linux_hw_breakpoint comp_bp;
  comp_bp.address = (unsigned int)addr;
  comp_bp.type = type;
  return sparc_linux_remove_hw_breakpoint_1(comp_bp);
}

static int sparc_linux_insert_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len,
				 enum target_hw_bp_type type, struct expression *cond)
{
  /*TODO: use length to create a mask*/
  return sparc_linux_insert_hw_breakpoint_1(sparc_place_addr(addr), type, 0);
}

/* Insert a new masked watchpoint at ADDR using the mask MASK.
   RW may be hw_read for a read watchpoint, hw_write for a write watchpoint
   or hw_access for an access watchpoint.  Returns 0 for success, 1 if
   masked watchpoints are not supported, -1 for failure.  */
static int sparc_linux_insert_mask_watchpoint (struct target_ops *ops, CORE_ADDR addr, 
				  CORE_ADDR mask, enum target_hw_bp_type type)
{
  return sparc_linux_insert_hw_breakpoint_1(sparc_place_addr(addr), type, (unsigned int)mask);
}
	  
	/* Remove a masked watchpoint at ADDR with the mask MASK.
   RW may be hw_read for a read watchpoint, hw_write for a write watchpoint
   or hw_access for an access watchpoint.  Returns 0 for success, non-zero
   for failure.  */
static int sparc_linux_remove_mask_watchpoint (struct target_ops *ops, CORE_ADDR addr, 
				  CORE_ADDR mask, enum target_hw_bp_type type)
{
  struct sparc_linux_hw_breakpoint comp_bp; 			//comparator breakpoint
  comp_bp.address = (unsigned int)addr;
  comp_bp.type = type;
  return sparc_linux_remove_hw_breakpoint_1(comp_bp);
}

/* Returns the number of debug registers needed to watch the given
   memory region, or zero if not supported.  */
static int sparc_linux_region_ok_for_hw_watchpoint (struct target_ops *ops,
					   CORE_ADDR addr, int len)
{
	   return 1;
}
  
/* Return non-zero if ADDR is within the range of a watchpoint spanning
   LENGTH bytes beginning at START.  */
static int sparc_linux_watchpoint_addr_within_range (struct target_ops *ops,
					    CORE_ADDR addr, CORE_ADDR start, int len)
{
			return 1;				
}
	  
/* Returns non-zero if we were stopped by a hardware watchpoint (memory read or
write).  Only the INFERIOR_PTID task is being queried. 
 *preferrably this function should use sparc_linux_stopped_data_address but this is not supported at the moment*/
static int sparc_linux_stopped_by_watchpoint (struct target_ops *ops){
	siginfo_t siginfo;
	struct sparc_linux_hw_breakpoint comp_bp;
	struct sparc_linux_hw_breakpoint *bp;
	struct sparc_linux_inferior_bps *inf_bps;
	//get siginfo
	int inf_pid = ptid_get_pid(inferior_ptid);
	ptrace(PTRACE_GETSIGINFO, inf_pid, 0,&siginfo);
	if(siginfo.si_signo != SIGTRAP)
		return 0;
		
	//get list of breakpoints 
	inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);
	
	//get the breakpoint that triggered the trap
	comp_bp.address = (unsigned int)siginfo.si_addr;
	comp_bp.type = hw_execute;		
	bp = get_breakpoint_in_inf_list(inf_bps, comp_bp);
	
	//if the breakpoint does not exist in the inferior, assume a watchpoint triggered the trap.
	if(bp == NULL)
		return 1;
		
	return 0;
}
/* Return non-zero if target knows the data address which triggered this
   target_stopped_by_watchpoint, in such case place it to *ADDR_P.  Only the
   INFERIOR_PTID task is being queried.  */
static int sparc_linux_stopped_data_address (struct target_ops *ops, CORE_ADDR *addr_p){
	 //The target can for now not determine which data address triggered a watchpoint
	 return 0;
}

/*Free the breakpoints used by the ended process*/
static void sparc_linux_forget_process (pid_t pid){
	
	struct sparc_linux_inferior_bps *inf_bps, *p;
	int i, j;
	
	//find the breakpoint list for the process
	for (i = 0; VEC_iterate(inf_bp_list, inf_list, i, p); i++){
	  if(p->id.pid == pid){
		  inf_bps = p;
		  break;
	  }
		
	}
	if(inf_bps == NULL)
		return;
	//free all the breakpoints
	for(j = 0; j <SPARC_MAX_HW_BPS; j++){
		if(inf_bps->bps[j] != NULL){
			xfree(inf_bps->bps[j]);
			inf_bps->bps[j] = NULL;
		}
	}
	//free the breakpoint list
	VEC_ordered_remove(inf_bp_list, inf_list,  i);
}

void _initialize_sparc_linux_nat (void);

void
_initialize_sparc_linux_nat (void)
{
  struct target_ops *t;

  /* Fill in the generic GNU/Linux methods.  */
  t = linux_target ();

  sparc_fpregmap = &sparc32_bsd_fpregmap;

  /* Add our register access methods.  */
  t->to_fetch_registers = sparc_fetch_inferior_registers;
  t->to_store_registers = sparc_store_inferior_registers;
  t->to_can_use_hw_breakpoint = sparc_linux_can_use_hw_breakpoint;
  t->to_insert_hw_breakpoint = sparc_linux_insert_hw_breakpoint;
  t->to_remove_hw_breakpoint = sparc_linux_remove_hw_breakpoint;
  t->to_insert_watchpoint = sparc_linux_insert_watchpoint;
  t->to_remove_watchpoint = sparc_linux_remove_watchpoint;
  t->to_insert_mask_watchpoint = sparc_linux_insert_mask_watchpoint;
  t->to_remove_mask_watchpoint = sparc_linux_remove_mask_watchpoint;
  t->to_watchpoint_addr_within_range = sparc_linux_watchpoint_addr_within_range;
  t->to_region_ok_for_hw_watchpoint = sparc_linux_region_ok_for_hw_watchpoint;
  /* Register the target.  */
  linux_nat_add_target (t);
  /*override linux implemementation of these operations*/
  t->to_stopped_data_address = sparc_linux_stopped_data_address;
  t->to_stopped_by_watchpoint = sparc_linux_stopped_by_watchpoint;
  
  linux_nat_set_forget_process (t, sparc_linux_forget_process);	
}
