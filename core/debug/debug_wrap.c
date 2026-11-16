#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#endif
#include <string.h>

#include "debug_wrap.h"

#ifdef _WIN32
static HANDLE hMapFile = NULL;
#else
static int shm;
#endif

dbg_request_t* create_shared_mem()
{
    dbg_request_t* request = NULL;

#ifdef _WIN32
    hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(dbg_request_t), SHARED_MEM_NAME);

    if (hMapFile == 0)
    {
        return NULL;
    }

    request = (dbg_request_t*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dbg_request_t));

    if (request == NULL)
    {
        CloseHandle(hMapFile);
        return NULL;
    }
#else
    shm = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0777);

    if (shm == -1)
        return NULL;

    /* A new object is empty: mapping it without a size faults on first touch. */
    if (ftruncate(shm, sizeof(dbg_request_t)) != 0) {
        close(shm);
        shm_unlink(SHARED_MEM_NAME);
        return NULL;
    }

    request = mmap(NULL, sizeof(dbg_request_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);

    if (request == MAP_FAILED) {
        close(shm);
        shm_unlink(SHARED_MEM_NAME);
        return NULL;
    }
#endif

    memset(request, 0, sizeof(dbg_request_t));

    return request;
}

dbg_request_t *open_shared_mem()
{
    dbg_request_t *request;

#ifdef _WIN32
    hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEM_NAME);

    if (hMapFile == NULL)
        return NULL;

    request = (dbg_request_t *)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dbg_request_t));

    if (request == NULL)
    {
        CloseHandle(hMapFile);
        return NULL;
    }

#else
    shm = shm_open(SHARED_MEM_NAME, O_RDWR, 0777);

    if (shm == -1)
        return NULL;

    request = mmap(NULL, sizeof(dbg_request_t), PROT_READ|PROT_WRITE, MAP_SHARED, shm, 0);

    if (request == MAP_FAILED) {
        close(shm);
        shm_unlink(SHARED_MEM_NAME);
        return NULL;
    }
#endif

    return request;
}

void close_shared_mem(dbg_request_t **request, int do_unmap)
{
    memset(*request, 0, sizeof(dbg_request_t));

#ifdef _WIN32
    if (do_unmap) {
        UnmapViewOfFile(*request);
    }
    CloseHandle(hMapFile);
    hMapFile = NULL;
    *request = NULL;
#else
    munmap(*request, sizeof(dbg_request_t));
    close(shm);
    shm_unlink(SHARED_MEM_NAME);
#endif
}

int recv_dbg_event_ida(dbg_request_t* request, int wait)
{
    while (request && (request->dbg_active == 1 || request->dbg_events_count > 0))
    {
        for (int i = 0; i < MAX_DBG_EVENTS; ++i)
        {
            if (request && request->dbg_events[i].type != DBG_EVT_NO_EVENT)
            {
                request->dbg_events_count -= 1;
                return i;
            }
        }

        if (!wait)
            return -1;
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10 * 1000);
#endif
    }

    return -1;
}

void send_dbg_request(dbg_request_t *request, request_type_t type, int ignore_active)
{
    if (!request)
        return;

    request->req_type = type;

    if (ignore_active) {
        request->dbg_active = 1;
    }

    while (request && request->dbg_active == 1 && request->req_type != REQ_NO_REQUEST)
    {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10 * 1000);
#endif
    }
}
