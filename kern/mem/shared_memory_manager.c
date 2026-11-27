#include <inc/memlayout.h>
#include "shared_memory_manager.h"

#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/queue.h>
#include <inc/syscall.h>
#include <inc/environment_definitions.h>
#include <kern/cpu/cpu.h>
#include <kern/proc/user_environment.h>
#include "kheap.h"
#include "memory_manager.h"

void free_share(struct Share* ptrShare);
struct Share* alloc_share(int32 ownerID, char* shareName, uint32 size, uint8 isWritable);

/************************************************************************************/
//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//
//TODO: [PROJECT'26.IM#6] SHARED MEMORY - [0] GIVEN FUNCTIONS
//===========================
// [1] INITIALIZE SHARES:
//===========================
//Initialize the list and the corresponding lock
void sharing_init()
{
#if USE_KHEAP
	LIST_INIT(&AllShares.shares_list) ;
	init_kspinlock(&AllShares.shareslock, "shares lock");
	//init_sleeplock(&AllShares.sharessleeplock, "shares sleep lock");
#else
	panic("not handled when KERN HEAP is disabled");
#endif
}
//===========================
// [2] Allocate ShareHandle:
//===========================
//Allocate ShareHandle object from the local storage of the given env
//Return:
//	a) if found: new ShareHandle object
//	b) else: NULL
struct ShareHandle* allocate_share_handle(struct Env* e)
{
	struct ShareHandle* handle = LIST_FIRST(&(e->__fshl__));
	if (handle == NULL) return NULL;
	LIST_REMOVE(&(e->__fshl__), handle);
	return handle;
}
//===========================
// [3] Free ShareHandle:
//===========================
//Free ShareHandle object by adding it to the local storage of the given env
void free_share_handle(struct Env* e, struct ShareHandle* shareHandle)
{
	if (shareHandle == NULL) return ;
	memset(shareHandle, 0, sizeof(struct ShareHandle));
	LIST_INSERT_HEAD(&(e->__fshl__), shareHandle);
}
//=========================
// [4] Find Share Object:
//=========================
//Search for the given shared object in the "shareHandles" of the owner env
//Return:
//	a) if found: ID of the shared object
//	b) else: E_SHARED_MEM_NOT_EXISTS
int find_share(int32 ownerID, char* name)
{
#if USE_KHEAP
	struct Env* ownerEnv;
	envid2env(ownerID, &ownerEnv, 0);

	int ret = E_SHARED_MEM_NOT_EXISTS;
	struct ShareHandle* shrHandle = NULL;
	acquire_kspinlock(&(ownerEnv->shareHandles.handleslock));
	{
		LIST_FOREACH(shrHandle, &(ownerEnv->shareHandles.share_handles_list), ShareHandle)
		{
			//cprintf("shared var name = %s compared with %s\n", name, shr->name);
			if(strcmp(name, shrHandle->name)==0)
			{
				//cprintf("%s found\n", name);
				ret = shrHandle->ID;
				break;
			}
		}
	}
	release_kspinlock(&(ownerEnv->shareHandles.handleslock));
	return ret;
#else
	panic("not handled when KERN HEAP is disabled");
#endif
}
/************************************************************************************/


//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//

//=====================================
// [1] Alloc & Initialize Share Object:
//=====================================
//Allocates a new shared object and initialize its member
//It dynamically creates the "framesStorage"
//Return: allocatedObject (pointer to struct Share) passed by reference
struct Share* alloc_share(int32 ownerID, char* shareName, uint32 size, uint8 isWritable)
{
	//TODO: [PROJECT'26.IM#6] SHARED MEMORY - [1] alloc_share
	struct Share* share = kmalloc(sizeof(struct Share));
	if (share == NULL)
		return NULL;
	memset(share, 0, sizeof(struct Share));

	uint32 numOfPages = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
	share->framesStorage = kmalloc(numOfPages * sizeof(struct FrameInfo*));
	if (share->framesStorage == NULL)
	{
		kfree(share);
		return NULL;
	}
	memset(share->framesStorage, 0, numOfPages * sizeof(struct FrameInfo*));

	share->ownerID = ownerID;
	strncpy(share->name, shareName, NAMELEN - 1);
	share->size = size;
	share->references = 1;
	share->isWritable = isWritable;
	share->ID = (uint32)share & 0x7FFFFFFF;
	return share;
}


//=========================
// [2] Create Share Object:
//=========================
int create_shared_object(int32 ownerID, char* shareName, uint32 size, uint8 isWritable, void* virtual_address)
{
	//TODO: [PROJECT'26.IM#6] SHARED MEMORY - [4] create_shared_object
	struct Env* myenv = get_cpu_proc(); //The calling environment

	uint32 numOfPages = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
	uint32 i, mapped = 0;

	struct Share* share = alloc_share(ownerID, shareName, size, isWritable);
	if (share == NULL)
		return E_NO_SHARE;

	acquire_kspinlock(&(myenv->shareHandles.handleslock));
	struct ShareHandle* handle = allocate_share_handle(myenv);
	release_kspinlock(&(myenv->shareHandles.handleslock));
	if (handle == NULL)
	{
		kfree(share->framesStorage);
		kfree(share);
		return E_NO_SHARE_HANDLE;
	}
	strncpy(handle->name, shareName, NAMELEN - 1);
	handle->ID = share->ID;
	share->handle = handle;

	for (i = 0; i < numOfPages; i++)
	{
		struct FrameInfo* frame = NULL;
		if (allocate_frame(&frame) != 0)
			break;
		if (map_frame(myenv->env_page_directory, frame, (uint32)virtual_address + i * PAGE_SIZE, PERM_WRITEABLE | PERM_USER) != 0)
		{
			free_frame(frame);
			break;
		}
		share->framesStorage[i] = frame;
		mapped++;
	}

	int ret = 0;
	if (mapped != numOfPages)
		ret = E_NO_SHARE;
	else
	{
		acquire_kspinlock(&AllShares.shareslock);
		if (find_share(ownerID, shareName) != E_SHARED_MEM_NOT_EXISTS)
			ret = E_SHARED_MEM_EXISTS;
		else
			LIST_INSERT_TAIL(&AllShares.shares_list, share);
		release_kspinlock(&AllShares.shareslock);
	}

	if (ret != 0)
	{
		for (i = 0; i < mapped; i++)
			unmap_frame(myenv->env_page_directory, (uint32)virtual_address + i * PAGE_SIZE);
		acquire_kspinlock(&(myenv->shareHandles.handleslock));
		free_share_handle(myenv, handle);
		release_kspinlock(&(myenv->shareHandles.handleslock));
		kfree(share->framesStorage);
		kfree(share);
		return ret;
	}

	acquire_kspinlock(&(myenv->shareHandles.handleslock));
	LIST_INSERT_TAIL(&(myenv->shareHandles.share_handles_list), handle);
	release_kspinlock(&(myenv->shareHandles.handleslock));

	return share->ID;
}

//==============================
// [3] Get Info of Share Object:
//==============================
int get_share_info(int32 ownerID, char* shareName, int *size)
{
	//TODO: [PROJECT'26.IM#6] SHARED MEMORY - [2] get_share_info
	int ID = find_share(ownerID, shareName);
	*size = 0;
	if (ID == E_SHARED_MEM_NOT_EXISTS)
		return E_SHARED_MEM_NOT_EXISTS;

	struct Share* share = NULL;
	acquire_kspinlock(&AllShares.shareslock);
	LIST_FOREACH(share, &AllShares.shares_list, Share)
	{
		if (share->ID == ID)
		{
			*size = share->size;
			break;
		}
	}
	release_kspinlock(&AllShares.shareslock);

	if (share == NULL)
		return E_SHARED_MEM_NOT_EXISTS;
	return ID;
}

//======================
// [4] Get Share Object:
//======================
int get_shared_object(int shareID, void* virtual_address)
{
	//TODO: [PROJECT'26.IM#6] SHARED MEMORY - [6] get_shared_object
	struct Env* myenv = get_cpu_proc(); //The calling environment

	struct Share* share = NULL;
	int ret = E_SHARED_MEM_NOT_EXISTS;

	acquire_kspinlock(&AllShares.shareslock);
	LIST_FOREACH(share, &AllShares.shares_list, Share)
	{
		if (share->ID == shareID)
			break;
	}

	if (share != NULL)
	{
		uint32 numOfPages = ROUNDUP(share->size, PAGE_SIZE) / PAGE_SIZE;
		uint32 perm = PERM_USER;
		uint32 i;
		if (share->isWritable)
			perm |= PERM_WRITEABLE;

		ret = 0;
		for (i = 0; i < numOfPages; i++)
		{
			if (map_frame(myenv->env_page_directory, share->framesStorage[i], (uint32)virtual_address + i * PAGE_SIZE, perm) != 0)
			{
				while (i > 0)
				{
					i--;
					unmap_frame(myenv->env_page_directory, (uint32)virtual_address + i * PAGE_SIZE);
				}
				ret = E_NO_MEM;
				break;
			}
		}
		if (ret == 0)
			share->references++;
	}
	release_kspinlock(&AllShares.shareslock);

	return ret;
}

//==================================================================================//
//============================== BONUS FUNCTIONS ===================================//
//==================================================================================//
//=========================
// [1] Delete Share Object:
//=========================
//delete the given shared object from the "shares_list"
//it should free its framesStorage and the share object itself
void free_share(struct Share* ptrShare)
{
	//TODO: [PROJECT'26.BONUS#4] EXIT #2 - [4] free_share
	//Your code is here
	//Comment the following line
	panic("free_share() is not implemented yet...!!");
}

//=========================
// [2] Free Share Object:
//=========================
int delete_shared_object(struct Env* e, int shareID, void *startVA)
{
	//TODO: [PROJECT'26.BONUS#4] EXIT #2 - [5] delete_shared_object
	//Your code is here
	//Comment the following line
	panic("delete_shared_object() is not implemented yet...!!");
}
