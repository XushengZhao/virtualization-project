
#include <vmm/vmx.h>
#include <inc/error.h>
#include <vmm/vmexits.h>
#include <vmm/ept.h>
#include <inc/x86.h>
#include <inc/assert.h>
#include <kern/pmap.h>
#include <kern/console.h>
#include <kern/kclock.h>
#include <kern/multiboot.h>
#include <inc/string.h>
#include <inc/stdio.h>
#include <kern/syscall.h>
#include <kern/env.h>
#include <kern/cpu.h>

static int vmdisk_number = 0; // this number assign to the vm
int vmx_get_vmdisk_number()
{
	return vmdisk_number;
}

void vmx_incr_vmdisk_number()
{
	vmdisk_number++;
}
bool find_msr_in_region(uint32_t msr_idx, uintptr_t *area, int area_sz, struct vmx_msr_entry **msr_entry)
{
	struct vmx_msr_entry *entry = (struct vmx_msr_entry *)area;
	int i;
	for (i = 0; i < area_sz; ++i)
	{
		if (entry->msr_index == msr_idx)
		{
			*msr_entry = entry;
			return true;
		}
	}
	return false;
}

bool handle_interrupt_window(struct Trapframe *tf, struct VmxGuestInfo *ginfo, uint32_t host_vector)
{
	uint64_t rflags;
	uint32_t procbased_ctls_or;

	procbased_ctls_or = vmcs_read32(VMCS_32BIT_CONTROL_PROCESSOR_BASED_VMEXEC_CONTROLS);

	// disable the interrupt window exiting
	procbased_ctls_or &= ~(VMCS_PROC_BASED_VMEXEC_CTL_INTRWINEXIT);

	vmcs_write32(VMCS_32BIT_CONTROL_PROCESSOR_BASED_VMEXEC_CONTROLS,
				 procbased_ctls_or);
	// write back the host_vector, which can insert a virtual interrupt
	vmcs_write32(VMCS_32BIT_CONTROL_VMENTRY_INTERRUPTION_INFO, host_vector);
	return true;
}
bool handle_interrupts(struct Trapframe *tf, struct VmxGuestInfo *ginfo, uint32_t host_vector)
{
	uint64_t rflags;
	uint32_t procbased_ctls_or;
	rflags = vmcs_read64(VMCS_GUEST_RFLAGS);

	if (!(rflags & (0x1 << 9)))
	{ // we have to wait the interrupt window open
		// get the interrupt info

		procbased_ctls_or = vmcs_read32(VMCS_32BIT_CONTROL_PROCESSOR_BASED_VMEXEC_CONTROLS);

		// disable the interrupt window exiting
		procbased_ctls_or |= VMCS_PROC_BASED_VMEXEC_CTL_INTRWINEXIT;

		vmcs_write32(VMCS_32BIT_CONTROL_PROCESSOR_BASED_VMEXEC_CONTROLS,
					 procbased_ctls_or);
	}
	else
	{ // revector the host vector to the guest vector

		vmcs_write32(VMCS_32BIT_CONTROL_VMENTRY_INTERRUPTION_INFO, host_vector);
	}
	return true;
}

bool handle_rdmsr(struct Trapframe *tf, struct VmxGuestInfo *ginfo)
{
	uint64_t msr = tf->tf_regs.reg_rcx;
	if (msr == EFER_MSR)
	{
		// TODO: setup msr_bitmap to ignore EFER_MSR
		uint64_t val;
		struct vmx_msr_entry *entry;
		bool r = find_msr_in_region(msr, ginfo->msr_guest_area, ginfo->msr_count, &entry);
		assert(r);
		val = entry->msr_value;

		tf->tf_regs.reg_rdx = val << 32;
		tf->tf_regs.reg_rax = val & 0xFFFFFFFF;

		tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
		return true;
	}

	return false;
}

bool handle_wrmsr(struct Trapframe *tf, struct VmxGuestInfo *ginfo)
{
	uint64_t msr = tf->tf_regs.reg_rcx;
	if (msr == EFER_MSR)
	{

		uint64_t cur_val, new_val;
		struct vmx_msr_entry *entry;
		bool r =
			find_msr_in_region(msr, ginfo->msr_guest_area, ginfo->msr_count, &entry);
		assert(r);
		cur_val = entry->msr_value;

		new_val = (tf->tf_regs.reg_rdx << 32) | tf->tf_regs.reg_rax;
		if (BIT(cur_val, EFER_LME) == 0 && BIT(new_val, EFER_LME) == 1)
		{
			// Long mode enable.
			uint32_t entry_ctls = vmcs_read32(VMCS_32BIT_CONTROL_VMENTRY_CONTROLS);
			// entry_ctls |= VMCS_VMENTRY_x64_GUEST;
			vmcs_write32(VMCS_32BIT_CONTROL_VMENTRY_CONTROLS,
						 entry_ctls);
		}

		entry->msr_value = new_val;
		tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
		return true;
	}

	return false;
}

bool handle_eptviolation(uint64_t *eptrt, struct VmxGuestInfo *ginfo)
{
	uint64_t gpa = vmcs_read64(VMCS_64BIT_GUEST_PHYSICAL_ADDR);
	int r;
	if (gpa < 0xA0000 || (gpa >= 0x100000 && gpa < ginfo->phys_sz))
	{
		// Allocate a new page to the guest.
		struct PageInfo *p = page_alloc(0);
		if (!p)
		{
			cprintf("vmm: handle_eptviolation: Failed to allocate a page for guest---out of memory.\n");
			return false;
		}
		p->pp_ref += 1;
		r = ept_map_hva2gpa(eptrt,
							page2kva(p), (void *)ROUNDDOWN(gpa, PGSIZE), __EPTE_FULL, 0);
		assert(r >= 0);
		/* cprintf("EPT violation for gpa:%x mapped KVA:%x\n", gpa, page2kva(p)); */
		return true;
	}
	else if (gpa >= CGA_BUF && gpa < CGA_BUF + PGSIZE)
	{
		// FIXME: This give direct access to VGA MMIO region.
		r = ept_map_hva2gpa(eptrt,
							(void *)(KERNBASE + CGA_BUF), (void *)CGA_BUF, __EPTE_FULL, 0);
		assert(r >= 0);
		return true;
	}
	cprintf("vmm: handle_eptviolation: Case 2, gpa %x\n", gpa);
	return false;
}

bool handle_ioinstr(struct Trapframe *tf, struct VmxGuestInfo *ginfo)
{
	static int port_iortc;

	uint64_t qualification = vmcs_read64(VMCS_VMEXIT_QUALIFICATION);
	int port_number = (qualification >> 16) & 0xFFFF;
	bool is_in = BIT(qualification, 3);
	bool handled = false;

	// handle reading physical memory from the CMOS.
	if (port_number == IO_RTC)
	{
		if (!is_in)
		{
			port_iortc = tf->tf_regs.reg_rax;
			handled = true;
		}
	}
	else if (port_number == IO_RTC + 1)
	{
		if (is_in)
		{
			if (port_iortc == NVRAM_BASELO)
			{
				tf->tf_regs.reg_rax = 640 & 0xFF;
				handled = true;
			}
			else if (port_iortc == NVRAM_BASEHI)
			{
				tf->tf_regs.reg_rax = (640 >> 8) & 0xFF;
				handled = true;
			}
			else if (port_iortc == NVRAM_EXTLO)
			{
				tf->tf_regs.reg_rax = ((ginfo->phys_sz / 1024) - 1024) & 0xFF;
				handled = true;
			}
			else if (port_iortc == NVRAM_EXTHI)
			{
				tf->tf_regs.reg_rax = (((ginfo->phys_sz / 1024) - 1024) >> 8) & 0xFF;
				handled = true;
			}
		}
	}
	if (handled)
	{
		tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
		return true;
	}
	else
	{
		cprintf("%x %x\n", qualification, port_iortc);
		return false;
	}
}

// Emulate a cpuid instruction.
// It is sufficient to issue the cpuid instruction here and collect the return value.
// You can store the output of the instruction in Trapframe tf,
//  but you should hide the presence of vmx from the guest if processor features are requested.
//
// Return true if the exit is handled properly, false if the VM should be terminated.
//
// Finally, you need to increment the program counter in the trap frame.
//
// Hint: The TA's solution does not hard-code the length of the cpuid instruction.
bool handle_cpuid(struct Trapframe *tf, struct VmxGuestInfo *ginfo)
{
	// --- LAB 3 ---
	uint32_t info, eax, ebx, ecx, edx;

	// determine the info value to use based on the value of rax in the trapframe
	info = tf->tf_regs.reg_rax;
	cpuid(info, &eax, &ebx, &ecx, &edx);

	// if info == 1, processor features were requested. we want to hide the presence of vmx from
	// the guest if this is the case
	// 0x20 is 100000. ~ it to set all bits but the 5th one to 0
	// then bitwise AND with ecx to zero out the 5th bit while keeping
	// all other bits the same
	if (info)
	{
		ecx &= ~0x20U;
	}

	// then store the output in the trapframe
	tf->tf_regs.reg_rax = eax;
	tf->tf_regs.reg_rbx = ebx;
	tf->tf_regs.reg_rcx = ecx;
	tf->tf_regs.reg_rdx = edx;

	// update the instruction pointer
	tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
	return true;
}

// Handle vmcall traps from the guest.
// We currently support 3 traps: read the virtual e820 map,
//   and use host-level IPC (send andrecv).
//
// Return true if the exit is handled properly, false if the VM should be terminated.
//
// Finally, you need to increment the program counter in the trap frame.
//
// Hint: The TA's solution does not hard-code the length of the cpuid instruction.//

bool handle_vmcall(struct Trapframe *tf, struct VmxGuestInfo *gInfo, uint64_t *eptrt)
{
	bool handled = false;
	multiboot_info_t mbinfo;
	int perm, r;
	void *gpa_pg, *hva_pg;
	envid_t to_env;
	uint32_t val;
	// phys address of the multiboot map in the guest.
	uint64_t multiboot_map_addr = 0x6000;
	switch (tf->tf_regs.reg_rax)
	{
	case VMX_VMCALL_MBMAP:
		/* Hint: */
		// Craft a multiboot (e820) memory map for the guest.
		//
		// Create three  memory mapping segments: 640k of low mem, the I/O hole (unusable), and
		//   high memory (phys_size - 1024k).
		//
		// Once the map is ready, find the kernel virtual address of the guest page (if present),
		//   or allocate one and map it at the multiboot_map_addr (0x6000).
		// Copy the mbinfo and memory_map_t (segment descriptions) into the guest page, and return
		//   a pointer to this region in rbx (as a guest physical address).
		/* Your code here */

		// -- LAB 3 --

		// this involves creating a "fake" memory map, stored in the mbinfo struct, to give to the guest
		// first wipe the mbinfo struct to make sure there is no garbage data there
		memset(&mbinfo, 0, sizeof(mbinfo));
		// we are creating a memroy map, so set the flags appropriately
		mbinfo.flags |= MB_FLAG_MMAP;
		// we are going to create 3 memory mapping segments
		mbinfo.mmap_length = 3 * sizeof(memory_map_t);
		// set the address of the location to copy the mapping segments. they will come just after
		// the mbinfo struct
		mbinfo.mmap_addr = multiboot_map_addr + sizeof(mbinfo);

		// now create and fill in the memory_map_t's for the three mapping segments
		// in memory_map_t, base_addr_low/base_addr_high and length_low/length_high are used to
		// store 64-bit values in 32-bit variables. *_low should store the lower 32 bits and *_high
		// should store the upper 32 bits.
		memory_map_t lomap, iohole, himap;

		// base addresses of each segment (from assignment document):
		// - low memory: 0
		// - IO hole: 640k (right after low memory)
		// - high memory: 1024k (right after the IO hole)

		// set up low mem
		memset(&lomap, 0, sizeof(lomap));
		lomap.length_low = 640 * 1024; // 640k
		lomap.size = sizeof(memory_map_t);
		lomap.type = MB_TYPE_USABLE;

		// set up io hole
		memset(&iohole, 0, sizeof(iohole));
		iohole.base_addr_low = 640 * 1024;
		iohole.length_low = (1024 * 1024) - (640 * 1024); // 1024k - 640k from the low memory
		iohole.size = sizeof(memory_map_t);
		iohole.type = MB_TYPE_RESERVED; // unusable

		// set up high mem
		memset(&himap, 0, sizeof(himap));
		himap.size = sizeof(memory_map_t);
		himap.type = MB_TYPE_USABLE;
		himap.base_addr_low = 1024 * 1024;					  // 1024k
		uint64_t himap_addr = gInfo->phys_sz - (1024 * 1024); // get the offset for this region
		// then make sure to handle both the lower and upper 32 bits
		himap.length_low = (uint32_t)himap_addr;
		himap.length_high = (uint32_t)(himap_addr >> 32);

		// copy the maps to guest memory. we first have to look up the host kernel virtual address
		// corresponding to multiboot_map_addr (which is a physical address in the guest.)
		// and allocate the page there if it doesn't exist yet
		void *hva = NULL;
		ept_gpa2hva(eptrt, (void *)multiboot_map_addr, &hva);
		// if the hva doesn't exist, allocate and map it
		if (!hva)
		{
			struct PageInfo *p = page_alloc(0);
			p->pp_ref += 1;
			hva = page2kva(p); // get the kernel virtual address for the page we just allocated
			// map the hva to multiboot_map_addr in the guest
			r = ept_map_hva2gpa(eptrt, hva, (void *)multiboot_map_addr, __EPTE_FULL, 0);
			if (r < 0)
			{
				return r;
			}
		}

		// then, copy the mapping structures into that page
		memcpy(hva, &mbinfo, sizeof(mbinfo));
		hva += sizeof(mbinfo);
		memcpy(hva, &lomap, sizeof(memory_map_t));
		hva += sizeof(memory_map_t);
		memcpy(hva, &iohole, sizeof(memory_map_t));
		hva += sizeof(memory_map_t);
		memcpy(hva, &himap, sizeof(memory_map_t));

		// set rbx to the multiboot region
		tf->tf_regs.reg_rbx = multiboot_map_addr;

		// and indicate that we've handled the exit
		handled = true;
		break;
	case VMX_VMCALL_IPCSEND:
		/* Hint: */
		// Issue the sys_ipc_send call to the host.
		//
		// If the requested environment is the HOST FS, this call should
		//  do this translation.
		//
		// The input should be a guest physical address; you will need to convert
		//  this to a host virtual address for the IPC to work properly.
		//  Then you should call sys_ipc_try_send()
		/* Your code here */
		enum EnvType dstenv_type = tf->tf_regs.reg_rbx;
		if (dstenv_type != ENV_TYPE_FS)
		{
			cprint("handle_vmcall, VMX_VMCALL_IPCSEND: dstenv_type = %e,", dstenv_type);
			return E_INVAL;
		}
		envid_t dstenv = -1;
		for (int i = 0; i < NENV; i++)
		{
			if (envs[i].env_type == ENV_TYPE_FS)
			{
				dstenv = i;
				break;
			}
		}
		if (dstenv == -1)
		{
			cprint("handle_vmcall, VMX_VMCALL_IPCSEND: no FS ENV found");
			return E_INVAL;
		}
		uint32_t value = tf->tf_regs.reg_rcx;
		void *pa = tf->tf_regs.reg_rdx;
		unsigned perm = tf->tf_regs.reg_rsi;
		void *hva = 0;
		ept_gpa2hva(eptrt, pa, &hva);
		if (hva == 0)
		{
			cprint("handle_vmcall, VMX_VMCALL_IPCSEND: gpa2hva failed");
			return -E_INVAL;
		}
		tf->tf_regs.reg_rax = sys_ipc_try_send(dstenv, value, hva, perm);
	
		handled = true;
		break;

	case VMX_VMCALL_IPCRECV:
		// Issue the sys_ipc_recv call for the guest.
		// NB: because recv can call schedule, clobbering the VMCS,
		// you should go ahead and increment rip before this call.
		/* Your code here */
		tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
		tf->tf_regs.reg_rax = sys_ipc_recv(((void *)tf->tf_regs.reg_rbx));
		tf->tf_regs.reg_rsi = curenv->env_ipc_value;

		break;
	case VMX_VMCALL_LAPICEOI:
		lapic_eoi();
		handled = true;
		break;
	case VMX_VMCALL_BACKTOHOST:
		cprintf("Now back to the host, VM halt in the background, run vmmanager to resume the VM.\n");
		curenv->env_status = ENV_NOT_RUNNABLE; // mark the guest not runable
		ENV_CREATE(user_sh, ENV_TYPE_USER);	   // create a new host shell
		handled = true;
		break;
	case VMX_VMCALL_GETDISKIMGNUM: // alloc a number to guest
		tf->tf_regs.reg_rax = vmdisk_number;
		handled = true;
		break;
	}
	if (handled)
	{
		/* Advance the program counter by the length of the vmcall instruction.
		 *
		 * Hint: The solution does not hard-code the length of the vmcall instruction.
		 */
		/* Your code here */
		// --- LAB 3 --
		tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
	}
	return handled;
}
