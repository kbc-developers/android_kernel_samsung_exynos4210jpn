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
#include "mali_kernel_core.h"
#include "mali_osk.h"
#include "mali_osk_list.h"
#include "mali_scheduler.h"
#include "mali_pp.h"
#include "mali_pp_job.h"
#include "mali_group.h"
#include "mali_cluster.h"

/* Maximum of 8 PP cores (a group can only have maximum of 1 PP core) */
#define MALI_MAX_NUMBER_OF_PP_GROUPS 8

enum mali_pp_slot_state
{
	MALI_PP_SLOT_STATE_IDLE,
	MALI_PP_SLOT_STATE_WORKING,
};

/* A render slot is an entity which jobs can be scheduled onto */
struct mali_pp_slot
{
	struct mali_group *group;
	/*
	 * We keep track of the state here as well as in the group object
	 * so we don't need to take the group lock so often (and also avoid clutter with the working lock)
	 */
	enum mali_pp_slot_state state;
};

static u32 pp_version = 0;
static _MALI_OSK_LIST_HEAD(job_queue);                          /* List of jobs with some unscheduled work */
static struct mali_pp_slot slots[MALI_MAX_NUMBER_OF_PP_GROUPS];
static u32 num_slots = 0;
static u32 num_slots_idle = 0;

/* Variables to allow safe pausing of the scheduler */
static _mali_osk_lock_t *pp_scheduler_working_lock = NULL;
static u32 pause_count = 0;

static _mali_osk_lock_t *pp_scheduler_lock = NULL;
/* Contains tid of thread that locked the scheduler or 0, if not locked */
MALI_DEBUG_CODE(static u32 pp_scheduler_lock_owner = 0);

_mali_osk_errcode_t mali_pp_scheduler_initialize(void)
{
	u32 i;

	_MALI_OSK_INIT_LIST_HEAD(&job_queue);

	pp_scheduler_lock = _mali_osk_lock_init(_MALI_OSK_LOCKFLAG_ORDERED |_MALI_OSK_LOCKFLAG_NONINTERRUPTABLE, 0, _MALI_OSK_LOCK_ORDER_SCHEDULER);
	pp_scheduler_working_lock = _mali_osk_lock_init( _MALI_OSK_LOCKFLAG_NONINTERRUPTABLE, 0, 0); /* This lock is not ordered. */

	if (NULL == pp_scheduler_lock)
	{
		return _MALI_OSK_ERR_NOMEM;
	}

	/* Find all the available PP cores */
	/*for (i = 0; i < mali_kernel_core_get_cluster_count(); i++)*/
	for (i = 0; i < mali_cluster_get_glob_num_clusters(); i++)
	{
		u32 group_id = 0;
		struct mali_cluster *curr_cluster = mali_cluster_get_global_cluster(i);/*mali_kernel_core_get_cluster(i);*/
		struct mali_group *group = mali_cluster_get_group(curr_cluster, group_id);
		while (NULL != group)
		{
			struct mali_pp_core *pp_core = mali_group_get_pp_core(group);
			if (NULL != pp_core)
			{
				if (0 == pp_version)
				{
					/* Retrieve PP version from first avaiable PP core */
					pp_version = mali_pp_core_get_version(pp_core);
				}
				slots[num_slots].group = group;
				slots[num_slots].state = MALI_PP_SLOT_STATE_IDLE;
				num_slots++;
				num_slots_idle++;
			}
			group_id++;
			group = mali_cluster_get_group(curr_cluster, group_id);
		}
	}

	return _MALI_OSK_ERR_OK;
}

void mali_pp_scheduler_terminate(void)
{
	_mali_osk_lock_term(pp_scheduler_working_lock);
	_mali_osk_lock_term(pp_scheduler_lock);
}

MALI_STATIC_INLINE void mali_pp_scheduler_lock(void)
{
	if(_MALI_OSK_ERR_OK != _mali_osk_lock_wait(pp_scheduler_lock, _MALI_OSK_LOCKMODE_RW))
	{
		/* Non-interruptable lock failed: this should never happen. */
		MALI_DEBUG_ASSERT(0);
	}
	MALI_DEBUG_PRINT(5, ("Mali PP scheduler: PP scheduler lock taken\n"));
	MALI_DEBUG_ASSERT(0 == pp_scheduler_lock_owner);
	MALI_DEBUG_CODE(pp_scheduler_lock_owner = _mali_osk_get_tid());
}

MALI_STATIC_INLINE void mali_pp_scheduler_unlock(void)
{
	MALI_DEBUG_PRINT(5, ("Mali PP scheduler: Releasing PP scheduler lock\n"));
	MALI_DEBUG_ASSERT(_mali_osk_get_tid() == pp_scheduler_lock_owner);
	MALI_DEBUG_CODE(pp_scheduler_lock_owner = 0);
	_mali_osk_lock_signal(pp_scheduler_lock, _MALI_OSK_LOCKMODE_RW);
}

#ifdef DEBUG
MALI_STATIC_INLINE void mali_pp_scheduler_assert_locked(void)
{
	MALI_DEBUG_ASSERT(_mali_osk_get_tid() == pp_scheduler_lock_owner);
}
#define MALI_ASSERT_PP_SCHEDULER_LOCKED() mali_pp_scheduler_assert_locked()
#else
#define MALI_ASSERT_PP_SCHEDULER_LOCKED()
#endif

MALI_STATIC_INLINE void mali_pp_scheduler_working_lock(void)
{
	_mali_osk_lock_wait(pp_scheduler_working_lock, _MALI_OSK_LOCKMODE_RW);
	MALI_DEBUG_PRINT(5, ("Mali PP scheduler: PP scheduler working lock taken\n"));
}

MALI_STATIC_INLINE void mali_pp_scheduler_working_unlock(void)
{
	MALI_DEBUG_PRINT(5, ("Mali PP scheduler: Releasing PP scheduler working lock\n"));
	_mali_osk_lock_signal(pp_scheduler_working_lock, _MALI_OSK_LOCKMODE_RW);
}

static void mali_pp_scheduler_schedule(void)
{
	u32 i;

	MALI_ASSERT_PP_SCHEDULER_LOCKED();

	if (0 < pause_count || 0 == num_slots_idle || _mali_osk_list_empty(&job_queue))
	{
		MALI_DEBUG_PRINT(4, ("Mali PP scheduler: Nothing to schedule (paused=%u, idle slots=%u)\n",
		                     pause_count, num_slots_idle));
		return; /* Nothing to do, so early out */
	}
#if MALI_PP_SCHEDULER_FORCE_NO_JOB_OVERLAP
	if ( (num_slots_idle<num_slots) )
	{
		MALI_DEBUG_PRINT(4, ("Mali PP scheduler: Job with %d subjobs not started, sine only %d/%d cores are available\n", job->sub_job_count, num_slots_idle,num_slots));
		return;
	}
#endif

	for (i = 0; (i < num_slots) && (0 < num_slots_idle); i++)
	{

		if (MALI_PP_SLOT_STATE_IDLE == slots[i].state)
		{
			struct mali_pp_job *job;
			u32 sub_job;

			if (_mali_osk_list_empty(&job_queue)) /* move this check down to where we know we have started all sub jobs for this job??? */
			{
				break; /* No more jobs to schedule, so early out */
			}

			job = _MALI_OSK_LIST_ENTRY(job_queue.next, struct mali_pp_job, list);
			MALI_DEBUG_ASSERT(mali_pp_job_has_unstarted_sub_jobs(job)); /* All jobs on the job_queue should have unstarted sub jobs */

#if MALI_PP_SCHEDULER_KEEP_SUB_JOB_STARTS_ALIGNED
			if ( (0==i) && (job->sub_job_count>num_slots_idle) && (num_slots_idle<num_slots) )
			{
				MALI_DEBUG_PRINT(4, ("Mali PP scheduler: Job with %d subjobs not started, sine only %d/%d cores are available\n", job->sub_job_count, num_slots_idle,num_slots));
				break;
			}
#endif

			sub_job = mali_pp_job_get_first_unstarted_sub_job(job);

			MALI_DEBUG_PRINT(3, ("Mali PP scheduler: Starting job %u (0x%08X) part %u/%u\n", mali_pp_job_get_id(job), job, sub_job + 1, mali_pp_job_get_sub_job_count(job)));
			if (_MALI_OSK_ERR_OK == mali_group_start_pp_job(slots[i].group, job, sub_job))
			{
				if (num_slots == num_slots_idle)
				{
					/* Hold the working lock as long as we are working on something */
					mali_pp_scheduler_working_lock();
				}

				/* Mark this sub job as started */
				mali_pp_job_mark_sub_job_started(job, sub_job);

				/* Mark slot as busy */
				slots[i].state = MALI_PP_SLOT_STATE_WORKING;
				num_slots_idle--;

				if (!mali_pp_job_has_unstarted_sub_jobs(job))
				{
					/*
					* All sub jobs have now started for this job, remove this job from the job queue.
					* The job will now only be referred to by the slots which are running it.
					* The last slot to complete will make sure it is returned to user space.
					*/
					_mali_osk_list_del(&job->list);
				}
			}
			else
			{
				MALI_DEBUG_PRINT(3, ("Mali PP scheduler: Failed to start PP job\n"));
			}
		}
	}
}

static void mali_pp_scheduler_return_job_to_user(struct mali_pp_job *job)
{
	_mali_osk_notification_t *notobj = _mali_osk_notification_create(_MALI_NOTIFICATION_PP_FINISHED, sizeof(_mali_uk_pp_job_finished_s));
	if (NULL != notobj)
	{
		u32 i;
		u32 sub_jobs = mali_pp_job_get_sub_job_count(job);
		mali_bool success = mali_pp_job_was_success(job);

		_mali_uk_pp_job_finished_s *jobres = notobj->result_buffer;
		_mali_osk_memset(jobres, 0, sizeof(_mali_uk_pp_job_finished_s)); /* @@@@ can be removed once we initialize all members in this struct */
		jobres->user_job_ptr = mali_pp_job_get_user_id(job);
		if (MALI_TRUE == success)
		{
			jobres->status = _MALI_UK_JOB_STATUS_END_SUCCESS;
		}
		else
		{
			jobres->status = _MALI_UK_JOB_STATUS_END_UNKNOWN_ERR;
		}

		for (i = 0; i < sub_jobs; i++)
		{
			jobres->perf_counter0[i] = mali_pp_job_get_perf_counter_value0(job, i);
			jobres->perf_counter1[i] = mali_pp_job_get_perf_counter_value1(job, i);
		}

		mali_session_send_notification(mali_pp_job_get_session(job), notobj);
	}
	else
	{
		MALI_PRINT_ERROR(("Mali PP scheduler: Unable to allocate notification object\n"));
	}

	mali_pp_job_delete(job);
}

void mali_pp_scheduler_do_schedule(void)
{
	mali_pp_scheduler_lock();

	mali_pp_scheduler_schedule();

	mali_pp_scheduler_unlock();
}

void mali_pp_scheduler_job_done(struct mali_group *group, struct mali_pp_job *job, u32 sub_job, mali_bool success)
{
	u32 i;
	mali_bool job_is_done;

	MALI_DEBUG_PRINT(3, ("Mali PP scheduler: Job %u (0x%08X) part %u/%u completed (%s)\n", mali_pp_job_get_id(job), job, sub_job + 1, mali_pp_job_get_sub_job_count(job), success ? "success" : "failure"));

	mali_pp_scheduler_lock();

	/* Find slot which was running this job */
	for (i = 0; i < num_slots; i++)
	{
		if (slots[i].group == group)
		{
			MALI_DEBUG_ASSERT(MALI_PP_SLOT_STATE_WORKING == slots[i].state);
			slots[i].state = MALI_PP_SLOT_STATE_IDLE;
			num_slots_idle++;
			mali_pp_job_mark_sub_job_completed(job, success);
		}
	}

	if (num_slots == num_slots_idle)
	{
		/* We are not working any more, so release the working lock */
		mali_pp_scheduler_working_unlock();
	}

	mali_pp_scheduler_schedule();

	job_is_done = mali_pp_job_is_complete(job);

	mali_pp_scheduler_unlock();

	if (job_is_done)
	{
		/* Send notification back to user space */
		MALI_DEBUG_PRINT(4, ("Mali PP scheduler: All parts completed for job %u (0x%08X)\n", mali_pp_job_get_id(job), job));
		mali_pp_scheduler_return_job_to_user(job);
	}
}

void mali_pp_scheduler_suspend(void)
{
	mali_pp_scheduler_lock();
	pause_count++; /* Increment the pause_count so that no more jobs will be scheduled */
	mali_pp_scheduler_unlock();

	mali_pp_scheduler_working_lock();
	/* We have now aquired the working lock, which means that we have successfully paused the scheduler */
	mali_pp_scheduler_working_unlock();
}

void mali_pp_scheduler_resume(void)
{
	mali_pp_scheduler_lock();
	pause_count--; /* Decrement pause_count to allow scheduling again (if it reaches 0) */
	if (0 == pause_count)
	{
		mali_pp_scheduler_schedule();
	}
	mali_pp_scheduler_unlock();
}

_mali_osk_errcode_t _mali_ukk_pp_start_job(_mali_uk_pp_start_job_s *args)
{
	struct mali_session_data *session;
	struct mali_pp_job *job;

	MALI_DEBUG_ASSERT_POINTER(args);

	if (NULL == args->ctx)
	{
		return _MALI_OSK_ERR_INVALID_ARGS;
	}

	session = (struct mali_session_data*)args->ctx;
	if (NULL == session)
	{
		return _MALI_OSK_ERR_FAULT;
	}

	job = mali_pp_job_create(session, args, mali_scheduler_get_new_id());
	if (NULL == job)
	{
		return _MALI_OSK_ERR_NOMEM;
	}

	if (_MALI_OSK_ERR_OK != mali_pp_job_check(job))
	{
		/* Not a valid job, return to user immediately */
		mali_pp_job_mark_sub_job_completed(job, MALI_FALSE); /* Flagging the job as failed. */
		mali_pp_scheduler_return_job_to_user(job); /* This will also delete the job object */
		return _MALI_OSK_ERR_OK; /* User is notified via a notification, so this call is ok */
	}

	mali_pp_scheduler_lock();

	_mali_osk_list_addtail(&job->list, &job_queue);

	MALI_DEBUG_PRINT(3, ("Mali PP scheduler: Job %u (0x%08X) with %u parts queued\n", mali_pp_job_get_id(job), job, mali_pp_job_get_sub_job_count(job)));

	mali_pp_scheduler_schedule();

	mali_pp_scheduler_unlock();

	return _MALI_OSK_ERR_OK;
}

_mali_osk_errcode_t _mali_ukk_get_pp_number_of_cores(_mali_uk_get_pp_number_of_cores_s *args)
{
	MALI_DEBUG_ASSERT_POINTER(args);
	MALI_CHECK_NON_NULL(args->ctx, _MALI_OSK_ERR_INVALID_ARGS);
	args->number_of_cores = num_slots;
	return _MALI_OSK_ERR_OK;
}

_mali_osk_errcode_t _mali_ukk_get_pp_core_version(_mali_uk_get_pp_core_version_s *args)
{
	MALI_DEBUG_ASSERT_POINTER(args);
	MALI_CHECK_NON_NULL(args->ctx, _MALI_OSK_ERR_INVALID_ARGS);
	args->version = pp_version;
	return _MALI_OSK_ERR_OK;
}

void mali_pp_scheduler_abort_session(struct mali_session_data *session)
{
	struct mali_pp_job *job, *tmp;
	int i;

	mali_pp_scheduler_lock();
	MALI_DEBUG_PRINT(3, ("Mali PP scheduler: Aborting all jobs from session 0x%08x\n", session));

	/* Check queue for jobs and remove */
	_MALI_OSK_LIST_FOREACHENTRY(job, tmp, &job_queue, struct mali_pp_job, list)
	{
		if (mali_pp_job_get_session(job) == session)
		{
			_mali_osk_list_del(&(job->list));

			if ( mali_pp_job_is_currently_rendering_and_if_so_abort_new_starts(job) )
			{
				/* The job is in the render pipeline, we can not delete it yet. */
				/* It will be deleted in the mali_group_abort_session() call below */
				MALI_DEBUG_PRINT(4, ("Mali PP scheduler: Keeping partially started PP job 0x%08x in queue\n", job));
				continue;
			}
			MALI_DEBUG_PRINT(4, ("Mali PP scheduler: Removing PP job 0x%08x from queue\n", job));
			mali_pp_job_delete(job);
		}
	}

	mali_pp_scheduler_unlock();

	/* Abort running jobs from this session */
	for (i = 0; i < num_slots; i++)
	{
		struct mali_group *group = slots[i].group;

		MALI_DEBUG_PRINT(5, ("PP sched abort: Looking at group 0x%08x\n", group));

		if (MALI_PP_SLOT_STATE_WORKING == slots[i].state)
		{
			MALI_DEBUG_PRINT(4, ("Mali PP scheduler: Aborting session 0x%08x from group 0x%08x\n", session, group));

			mali_group_abort_session(group, session);
		}
	}
}

#if MALI_STATE_TRACKING
u32 mali_pp_scheduler_dump_state(char *buf, u32 size)
{
	int n = 0;
	int i;

	n += _mali_osk_snprintf(buf + n, size - n, "PP:\n");
	n += _mali_osk_snprintf(buf + n, size - n, "\tQueue is %s\n", _mali_osk_list_empty(&job_queue) ? "empty" : "not empty");
	n += _mali_osk_snprintf(buf + n, size - n, "\n");

	for (i = 0; i < num_slots; i++)
	{
		n += mali_group_dump_state(slots[i].group, buf + n, size - n);
		n += _mali_osk_snprintf(buf + n, size - n, "\t\tState: %d\n", slots[i].state);
	}

	return n;
}
#endif
