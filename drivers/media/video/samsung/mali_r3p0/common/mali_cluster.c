/*
 * Copyright (C) 2010-2012 ARM Limited. All rights reserved.
 * 
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "mali_kernel_common.h"
#include "mali_cluster.h"
#include "mali_osk.h"
#include "mali_group.h"
#include "mali_l2_cache.h"

/* id of the last job that caused the cache to be invalidated */
static uint32_t last_invalidate_id = 0;

static struct mali_cluster *global_clusters[MALI_MAX_NUMBER_OF_CLUSTERS] = { NULL, NULL, NULL };
static u32 global_num_clusters = 0;

/**
 * The structure represents a render cluster
 * A render cluster is defined by all the cores that share the same Mali L2 cache
 */
struct mali_cluster
{
	struct mali_l2_cache_core *l2;
	u32 number_of_groups;
	struct mali_group* groups[MALI_MAX_NUMBER_OF_GROUPS_PER_CLUSTER];
};

struct mali_cluster *mali_cluster_create(struct mali_l2_cache_core *l2_cache)
{
	struct mali_cluster *cluster = NULL;

	cluster = _mali_osk_malloc(sizeof(struct mali_cluster));
	if (NULL != cluster)
	{
		_mali_osk_memset(cluster, 0, sizeof(struct mali_cluster));
		cluster->l2 = l2_cache; /* This cluster now owns this L2 cache object */

		if(global_num_clusters < MALI_MAX_NUMBER_OF_CLUSTERS-1)
		{
			MALI_DEBUG_PRINT(2, ("Mali cluster: set global cluster no %d from 0x%08X to 0x%08X\n", global_num_clusters, global_clusters[global_num_clusters], cluster));
			global_clusters[global_num_clusters] = cluster;
			MALI_DEBUG_PRINT(2, ("Mali cluster: global cluster no %d set to 0x%08X\n", global_num_clusters, global_clusters[global_num_clusters]));
			global_num_clusters++;
		}
		else
		{
			MALI_PRINT_ERROR(("Mali cluster: Wrong number of global clusters\n"));
		}

		return cluster;
	}

	return NULL;
}

void mali_cluster_add_group(struct mali_cluster *cluster, struct mali_group *group)
{
	MALI_DEBUG_ASSERT_POINTER(cluster);

	if (cluster->number_of_groups < MALI_MAX_NUMBER_OF_GROUPS_PER_CLUSTER)
	{
		/* This cluster now owns the group object */
		cluster->groups[cluster->number_of_groups] = group;
		cluster->number_of_groups++;
	}
}

void mali_cluster_delete(struct mali_cluster *cluster)
{
	u32 i;

	MALI_DEBUG_ASSERT_POINTER(cluster);

	/* Free all the resources we own */
	for (i = 0; i < cluster->number_of_groups; i++)
	{
		mali_group_delete(cluster->groups[i]);
	}

	if (NULL != cluster->l2)
	{
		mali_l2_cache_delete(cluster->l2);
	}

	if(global_num_clusters > 0)
	{
		u32 i,j;

		for(i=0; i<global_num_clusters; i++)
		{
			if(global_clusters[i] == cluster)
			{
				MALI_DEBUG_PRINT(2, ("Mali cluster: set global cluster no %d from 0x%08X to NULL\n", i, global_clusters[i]));
				global_clusters[i] = NULL;
				MALI_DEBUG_PRINT(2, ("Mali cluster: global cluster no %d set to 0x%08X\n", i, global_clusters[i]));
				for(j=i; j<global_num_clusters-1; j++)
				{
					global_clusters[j] = global_clusters[j+1];
				}
				global_clusters[global_num_clusters-1] = NULL;
			}
		}
		global_num_clusters--;
	}
	else
	{
		MALI_PRINT_ERROR(("Mali cluster: Wrong number of global clusters\n"));
	}

	_mali_osk_free(cluster);
}

void mali_cluster_reset(struct mali_cluster *cluster)
{
	u32 i;

	MALI_DEBUG_ASSERT_POINTER(cluster);

	/* Free all the resources we own */
	for (i = 0; i < cluster->number_of_groups; i++)
	{
		struct mali_group *group = cluster->groups[i];

		mali_group_reset(group);
	}

	if (NULL != cluster->l2)
	{
		mali_l2_cache_reset(cluster->l2);
	}
}

struct mali_l2_cache_core* mali_cluster_get_l2_cache_core(struct mali_cluster *cluster)
{
	MALI_DEBUG_ASSERT_POINTER(cluster);
	return cluster->l2;
}

struct mali_group *mali_cluster_get_group(struct mali_cluster *cluster, u32 index)
{
	MALI_DEBUG_ASSERT_POINTER(cluster);

	if (index <  cluster->number_of_groups)
	{
		return cluster->groups[index];
	}

	return NULL;
}

struct mali_cluster *mali_cluster_get_global_cluster(u32 index)
{
	if (MALI_MAX_NUMBER_OF_CLUSTERS >= index)
	{
		return global_clusters[index];
	}

	return NULL;
}

u32 mali_cluster_get_glob_num_clusters(void)
{
	return global_num_clusters;
}

void mali_cluster_set_glob_num_clusters(u32 num)
{
	global_num_clusters = num;
}

void mali_cluster_l2_cache_invalidate_all(struct mali_cluster *cluster, u32 id)
{
	MALI_DEBUG_ASSERT_POINTER(cluster);

	if (NULL != cluster->l2)
	{
		/* If the last cache invalidation was done by a job with a higher id we
		 * don't have to flush. Since user space will store jobs w/ their
		 * corresponding memory in sequence (first job #0, then job #1, ...),
		 * we don't have to flush for job n-1 if job n has already invalidated
		 * the cache since we know for sure that job n-1's memory was already
		 * written when job n was started. */
		if (last_invalidate_id > id)
		{
			return;
		}
		else
		{
			last_invalidate_id = id;
		}

		mali_l2_cache_invalidate_all(cluster->l2);
	}
}

void mali_cluster_l2_cache_invalidate_all_force(struct mali_cluster *cluster)
{
	MALI_DEBUG_ASSERT_POINTER(cluster);

	if (NULL != cluster->l2)
	{
		mali_l2_cache_invalidate_all(cluster->l2);
	}
}
