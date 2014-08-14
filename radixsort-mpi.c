
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include <mpi.h>

#include "radixsort.h"

#include "internal.h"

#include "internal-parallel.h"

/* mpi version of radix sort; 
 *
 * each caller provides the distributed array and number of items.
 * the sorted array is returned to the original array pointed to by 
 * mybase. (AKA no rebalancing is done.)
 *
 * NOTE: may need an api to return a balanced array!
 *
 * uses the same amount of temporary storage space for communication
 * and local sort. (this will be allocated via malloc)
 *
 *
 * */

static MPI_Datatype MPI_TYPE_PTRDIFF = 0;
struct crmpistruct {
    MPI_Datatype MPI_TYPE_RADIX;
    MPI_Comm comm;
    int NTask;
    int ThisTask;
};

static void _setup_radix_sort_mpi(struct crmpistruct * o, struct crstruct * d, MPI_Comm comm) {
    o->comm = comm;
    MPI_Comm_size(comm, &o->NTask);
    MPI_Comm_rank(comm, &o->ThisTask);

    if(MPI_TYPE_PTRDIFF == 0) {
        if(sizeof(ptrdiff_t) == sizeof(long long)) {
            MPI_TYPE_PTRDIFF = MPI_LONG_LONG;
        }
        if(sizeof(ptrdiff_t) == sizeof(long)) {
            MPI_TYPE_PTRDIFF = MPI_LONG;
        }
        if(sizeof(ptrdiff_t) == sizeof(int)) {
            MPI_TYPE_PTRDIFF = MPI_INT;
        }
    }

    MPI_Type_contiguous(d->rsize, MPI_BYTE, &o->MPI_TYPE_RADIX);
    MPI_Type_commit(&o->MPI_TYPE_RADIX);

}
static void _destroy_radix_sort_mpi(struct crmpistruct * o) {
    MPI_Type_free(&o->MPI_TYPE_RADIX);
}

static void _find_Pmax_Pmin_C(void * mybase, size_t mynmemb, 
        char * Pmax, char * Pmin, 
        ptrdiff_t * C,
        struct crstruct * d,
        struct crmpistruct * o);

static void _solve_for_layout_mpi (
        int NTask, 
        ptrdiff_t * C,
        ptrdiff_t * myT_CLT, 
        ptrdiff_t * myT_CLE, 
        ptrdiff_t * myT_C,
        MPI_Comm comm);
static struct TIMER {
    double time;
    char name[20];
} _TIMERS[20];

void radix_sort_mpi_report_last_run() {
    struct TIMER * tmr = _TIMERS;
    double last = tmr->time;
    tmr ++;
    while(0 != strcmp(tmr->name, "END")) {
        printf("%s: %g\n", tmr->name, tmr->time - last);
        last =tmr->time;
        tmr ++;
    }
}
void radix_sort_mpi(void * mybase, size_t mynmemb, size_t size,
        void (*radix)(const void * ptr, void * radix, void * arg), 
        size_t rsize, 
        void * arg, 
        MPI_Comm comm) {
    struct crstruct d;
    struct crmpistruct o;

    struct piter pi;

    size_t nmemb;

    _setup_radix_sort(&d, size, radix, rsize, arg);
    _setup_radix_sort_mpi(&o, &d, comm);

    char Pmax[d.rsize];
    char Pmin[d.rsize];

    char P[d.rsize * (o.NTask - 1)];

    ptrdiff_t C[o.NTask + 1];  /* desired counts */

    ptrdiff_t myCLT[o.NTask + 1]; /* counts of less than P */
    ptrdiff_t CLT[o.NTask + 1]; 

    ptrdiff_t myCLE[o.NTask + 1]; /* counts of less than or equal to P */
    ptrdiff_t CLE[o.NTask + 1]; 

    int SendCount[o.NTask];
    int SendDispl[o.NTask];
    int RecvCount[o.NTask];
    int RecvDispl[o.NTask];

    ptrdiff_t myT_CLT[2 * o.NTask];
    ptrdiff_t myT_CLE[2 * o.NTask];
    ptrdiff_t myT_C[2 * o.NTask];
    ptrdiff_t myC[o.NTask + 1];

    int iter = 0;
    int done = 0;
    char * buffer;
    int i;

    struct TIMER * tmr = _TIMERS;

    MPI_Allreduce(&mynmemb, &nmemb, 1, MPI_TYPE_PTRDIFF, MPI_SUM, o.comm);

    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "START"), tmr++);

    if(nmemb == 0) goto exec_empty_array;

    /* and sort the local array */
    radix_sort(mybase, mynmemb, d.size, d.radix, d.rsize, d.arg);

    MPI_Barrier(comm);
    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "FirstSort"), tmr++);

    _find_Pmax_Pmin_C(mybase, mynmemb, Pmax, Pmin, C, &d, &o);

    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "PmaxPmin"), tmr++);

    memset(P, 0, d.rsize * (o.NTask -1));

    piter_init(&pi, Pmin, Pmax, o.NTask - 1, &d);

    while(!done) {
        iter ++;
        piter_bisect(&pi, P);

        _histogram(P, o.NTask - 1, mybase, mynmemb, myCLT, myCLE, &d);

        MPI_Allreduce(myCLT, CLT, o.NTask + 1, 
                MPI_TYPE_PTRDIFF, MPI_SUM, o.comm);
        MPI_Allreduce(myCLE, CLE, o.NTask + 1, 
                MPI_TYPE_PTRDIFF, MPI_SUM, o.comm);

        piter_accept(&pi, P, C, CLT, CLE);
#if 0
        {
            int k;
            for(k = 0; k < o.NTask; k ++) {
                MPI_Barrier(o.comm);
                int i;
                if(o.ThisTask != k) continue;
                
                printf("P (%d): PMin %d PMax %d P ", 
                        o.ThisTask, 
                        *(int*) Pmin,
                        *(int*) Pmax
                        );
                for(i = 0; i < o.NTask - 1; i ++) {
                    printf(" %d ", ((int*) P) [i]);
                }
                printf("\n");

                printf("C (%d): ", o.ThisTask);
                for(i = 0; i < o.NTask + 1; i ++) {
                    printf("%d ", C[i]);
                }
                printf("\n");
                printf("CLT (%d): ", o.ThisTask);
                for(i = 0; i < o.NTask + 1; i ++) {
                    printf("%d ", CLT[i]);
                }
                printf("\n");
                printf("CLE (%d): ", o.ThisTask);
                for(i = 0; i < o.NTask + 1; i ++) {
                    printf("%d ", CLE[i]);
                }
                printf("\n");

            }
        }
#endif
        done = piter_all_done(&pi);
    }

    piter_destroy(&pi);

    _histogram(P, o.NTask - 1, mybase, mynmemb, myCLT, myCLE, &d);

    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "findP"), tmr++);

    /* transpose the matrix, could have been done with a new datatype */
    /*
    MPI_Alltoall(myCLT, 1, MPI_TYPE_PTRDIFF, 
            myT_CLT, 1, MPI_TYPE_PTRDIFF, o.comm);
    */
    MPI_Alltoall(myCLT + 1, 1, MPI_TYPE_PTRDIFF, 
            myT_CLT + o.NTask, 1, MPI_TYPE_PTRDIFF, o.comm);

    /*MPI_Alltoall(myCLE, 1, MPI_TYPE_PTRDIFF, 
            myT_CLE, 1, MPI_TYPE_PTRDIFF, o.comm); */
    MPI_Alltoall(myCLE + 1, 1, MPI_TYPE_PTRDIFF, 
            myT_CLE + o.NTask, 1, MPI_TYPE_PTRDIFF, o.comm);

    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "LayDistr"), tmr++);

    _solve_for_layout_mpi(o.NTask, C, myT_CLT, myT_CLE, myT_C, o.comm);

    myC[0] = 0;
    MPI_Alltoall(myT_C + o.NTask, 1, MPI_TYPE_PTRDIFF, 
            myC + 1, 1, MPI_TYPE_PTRDIFF, o.comm);

    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "LaySolve"), tmr++);

    buffer = malloc(d.size * mynmemb);

    for(i = 0; i < o.NTask; i ++) {
        SendCount[i] = myC[i + 1] - myC[i];
    }

    MPI_Alltoall(SendCount, 1, MPI_INT,
            RecvCount, 1, MPI_INT, o.comm);

    SendDispl[0] = 0;
    RecvDispl[0] = 0;
    for(i = 1; i < o.NTask; i ++) {
        SendDispl[i] = SendDispl[i - 1] + SendCount[i - 1];
        RecvDispl[i] = RecvDispl[i - 1] + RecvCount[i - 1];
        if(SendDispl[i] != myC[i]) {
            fprintf(stderr, "SendDispl error\n");
            abort();
        }
    }

#if 0
    {
        int k;
        for(k = 0; k < o.NTask; k ++) {
            MPI_Barrier(o.comm);

            if(o.ThisTask != k) continue;
            
            printf("P (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask - 1; i ++) {
                printf("%d ", ((int*) P) [i]);
            }
            printf("\n");

            printf("C (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask + 1; i ++) {
                printf("%d ", C[i]);
            }
            printf("\n");
            printf("CLT (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask + 1; i ++) {
                printf("%d ", CLT[i]);
            }
            printf("\n");
            printf("CLE (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask + 1; i ++) {
                printf("%d ", CLE[i]);
            }
            printf("\n");

            printf("MyC (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask + 1; i ++) {
                printf("%d ", myC[i]);
            }
            printf("\n");
            printf("MyCLT (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask + 1; i ++) {
                printf("%d ", myCLT[i]);
            }
            printf("\n");

            printf("MyCLE (%d): ", o.ThisTask);
            for(i = 0; i < o.NTask + 1; i ++) {
                printf("%d ", myCLE[i]);
            }
            printf("\n");

            printf("Send Count(%d): ", o.ThisTask);
            for(i = 0; i < o.NTask; i ++) {
                printf("%d ", SendCount[i]);
            }
            printf("\n");
            printf("My data(%d): ", o.ThisTask);
            for(i = 0; i < mynmemb; i ++) {
                printf("%d ", ((int*) mybase)[i]);
            }
            printf("\n");
        }
    }
#endif
    MPI_Datatype MPI_TYPE_DATA;
    MPI_Type_contiguous(d.size, MPI_BYTE, &MPI_TYPE_DATA);
    MPI_Type_commit(&MPI_TYPE_DATA);
    MPI_Alltoallv(
            mybase, SendCount, SendDispl, MPI_TYPE_DATA,
            buffer, RecvCount, RecvDispl, MPI_TYPE_DATA, 
            o.comm);
    MPI_Type_free(&MPI_TYPE_DATA);

    memcpy(mybase, buffer, mynmemb * d.size);
    free(buffer);

    MPI_Barrier(comm);
    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "Exchange"), tmr++);

    radix_sort(mybase, mynmemb, d.size, d.radix, d.rsize, d.arg);

    MPI_Barrier(comm);
    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "SecondSort"), tmr++);

exec_empty_array:
    (tmr->time = MPI_Wtime(), strcpy(tmr->name, "END"), tmr++);
    _destroy_radix_sort_mpi(&o);
}

static void _find_Pmax_Pmin_C(void * mybase, size_t mynmemb, 
        char * Pmax, char * Pmin, 
        ptrdiff_t * C,
        struct crstruct * d,
        struct crmpistruct * o) {
    memset(Pmax, 0, d->rsize);
    memset(Pmin, -1, d->rsize);

    char myPmax[d->rsize];
    char myPmin[d->rsize];

    size_t eachnmemb[o->NTask];
    char eachPmax[d->rsize * o->NTask];
    char eachPmin[d->rsize * o->NTask];
    int i;

    if(mynmemb > 0) {
        d->radix((char*) mybase + (mynmemb - 1) * d->size, myPmax, d->arg);
        d->radix(mybase, myPmin, d->arg);
    } else {
        memset(myPmin, 0, d->rsize);
        memset(myPmax, 0, d->rsize);
    }

    MPI_Allgather(&mynmemb, 1, MPI_TYPE_PTRDIFF, 
            eachnmemb, 1, MPI_TYPE_PTRDIFF, o->comm);
    MPI_Allgather(myPmax, 1, o->MPI_TYPE_RADIX, 
            eachPmax, 1, o->MPI_TYPE_RADIX, o->comm);
    MPI_Allgather(myPmin, 1, o->MPI_TYPE_RADIX, 
            eachPmin, 1, o->MPI_TYPE_RADIX, o->comm);


    C[0] = 0;
    for(i = 0; i < o->NTask; i ++) {
        C[i + 1] = C[i] + eachnmemb[i];
        if(eachnmemb[i] == 0) continue;

        if(d->compar(eachPmax + i * d->rsize, Pmax, d->rsize) > 0) {
            memcpy(Pmax, eachPmax + i * d->rsize, d->rsize);
        }
        if(d->compar(eachPmin + i * d->rsize, Pmin, d->rsize) < 0) {
            memcpy(Pmin, eachPmin + i * d->rsize, d->rsize);
        }
    }
}

static void _solve_for_layout_mpi (
        int NTask, 
        ptrdiff_t * C,
        ptrdiff_t * myT_CLT, 
        ptrdiff_t * myT_CLE, 
        ptrdiff_t * myT_C,
        MPI_Comm comm) {
    int NTask1 = NTask + 1;
    int i, j;
    int ThisTask;
    MPI_Comm_rank(comm, &ThisTask);

    /* See if this rank has no duplicates; if so, 
     * no need to participate in the solving ring */
    int my_nodup = 1;
    int nodup[NTask];
    for(i = 0; i < NTask; i ++) {
        if(myT_CLE[NTask + i] == myT_CLT[NTask + i]) {
            myT_C[NTask + i] = myT_CLT[NTask + i];
        } else {
            my_nodup = 0;
            break;
        }
    }
    MPI_Allgather(&my_nodup, 1, MPI_INT, nodup, 1, MPI_INT, comm);

    if(my_nodup == 0) {
        /* receive the left slab of myT_C from previous rank */
        if(ThisTask > 0) {
            MPI_Recv(myT_C, NTask, MPI_TYPE_PTRDIFF,
                    ThisTask - 1, 0xdead, comm, MPI_STATUS_IGNORE);
        } else {
            for(i = 0; i < NTask; i ++) {
                myT_C[i] = 0;
            }
            
        }

        /* first assume we just send according to myT_CLT */
        for(i = 0; i < NTask; i ++) {
            myT_C[NTask + i] = myT_CLT[NTask + i];
        }

        /* Solve for each receiving task i 
         *
         * this solves for GL_C[..., i + 1], which depends on GL_C[..., i]
         *
         * and we have GL_C[..., 0] == 0 by definition.
         *
         * this cannot be done in parallel wrt i because of the dependency. 
         *
         *  a solution is guaranteed because GL_CLE and GL_CLT
         *  brackes the total counts C (we've found it with the
         *  iterative counting.
         *
         * */

        ptrdiff_t sure = 0;

        /* how many will I surely receive? */
        for(j = 0; j < NTask; j ++) {
            ptrdiff_t recvcount = myT_C[NTask + j] - myT_C[j];
            sure += recvcount;
        }
        /* let's see if we have enough */
        ptrdiff_t deficit = C[ThisTask + 1] - C[ThisTask] - sure;

        for(j = 0; j < NTask; j ++) {
            /* deficit solved */
            if(deficit == 0) break;
            if(deficit < 0) {
                fprintf(stderr, "serious bug: more items than there should be: deficit=%ld\n", deficit);
                abort();
            }
            /* how much task j can supply ? */
            ptrdiff_t supply = myT_CLE[NTask + j] - myT_C[NTask + j];
            if(supply < 0) {
                fprintf(stderr, "serious bug: less items than there should be: supply =%ld\n", supply);
                abort();
            }
            if(supply <= deficit) {
                myT_C[NTask + j] += supply;
                deficit -= supply;
            } else {
                myT_C[NTask + j] += deficit;
                deficit = 0;
            }
        }

    }

    if(ThisTask < NTask - 1) {
        if(nodup[ThisTask + 1] == 0) {
            MPI_Send(myT_C + NTask, NTask, MPI_TYPE_PTRDIFF,
                    ThisTask + 1, 0xdead, comm);
        }
    }
#if 0
    for(i = 0; i < NTask; i ++) {
        for(j = 0; j < NTask + 1; j ++) {
            printf("%d %d %d, ", 
                    GL_CLT[i * NTask1 + j], 
                    GL_C[i * NTask1 + j], 
                    GL_CLE[i * NTask1 + j]);
        }
        printf("\n");
    }
#endif

}
