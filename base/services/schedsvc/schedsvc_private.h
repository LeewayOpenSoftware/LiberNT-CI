/*
 * Copyright 2014 Dmitry Timoshkov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_SCHEDSVC_PRIVATE_H__
#define __WINE_SCHEDSVC_PRIVATE_H__

void schedsvc_auto_start(void);
void add_job(const WCHAR *name);
void remove_job(const WCHAR *name);
void update_job(const WCHAR *name);
void check_task_state(void);
void add_process_to_queue(HANDLE hproc);
void update_process_status(DWORD pid);
BOOL get_next_runtime(LARGE_INTEGER *rt);
void check_task_time(void);
void load_at_tasks(void);
void check_missed_task_time(void);

/* Task Scheduler 2.0 engine (engine.c) */
void engine_start(void);
void engine_stop(void);
void engine_notify_logon(void);
void engine_notify_changed(void);
BOOL engine_valid_path(const WCHAR *path);
HRESULT engine_check_register(const WCHAR *path, const WCHAR *xml, DWORD flags, WCHAR **registrant);
void engine_lock(void);
void engine_unlock(void);
HRESULT engine_task_registered(const WCHAR *path, const WCHAR *registrant);
HRESULT engine_check_task_access(const WCHAR *path);
void engine_task_deleted(const WCHAR *path);
HRESULT engine_run(const WCHAR *path, DWORD n_args, const WCHAR **args, DWORD flags, DWORD session_id,
                   const WCHAR *user, GUID *guid);
HRESULT engine_stop_task(const WCHAR *path);
HRESULT engine_stop_instance(const GUID *guid);
HRESULT engine_enum_instances(const WCHAR *path, DWORD *n_guids, GUID **guids);
HRESULT engine_instance_info(const GUID *guid, WCHAR **path, DWORD *state, WCHAR **action, DWORD *pid);
HRESULT engine_last_run(const WCHAR *path, SYSTEMTIME *last, DWORD *code);
HRESULT engine_missed_runs(const WCHAR *path, DWORD *runs);
DWORD engine_task_state(const WCHAR *path, BOOL enabled);
HRESULT engine_runtimes(const WCHAR *path, const SYSTEMTIME *start, const SYSTEMTIME *end, DWORD n_requested,
                        DWORD *n_runtimes, SYSTEMTIME **runtimes);

#endif /* __WINE_SCHEDSVC_PRIVATE_H__ */
