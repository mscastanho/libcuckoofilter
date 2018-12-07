#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cuckoo_filter.h"

#define MAX_RANGE   256
#define MAX_INSERT  ((int) (MAX_RANGE*0.8))
#define MAX_RUNS    1

/* To compile run:
        gcc test_cuckoofilter.c -o testcf -L ./ -lbpfmap
*/
int main(){
    printf("Testing CuckooFilter\n");

    cuckoo_filter_t *cf, *loaded_cf;
    
    if(cuckoo_filter_new(&cf,MAX_RANGE,500,0) != CUCKOO_FILTER_OK){
        printf("Failed to create Cuckoo Filter\n");
        exit(1);
    }

    printf("Filter created successfully\n");

    int i,j,cnt,r,ret;
    CUCKOO_FILTER_RETURN res;
    time_t seed;
    int added[MAX_RANGE];

    srand((unsigned) time(&seed));
    memset(added,0,sizeof(added));

    /* Insert MAX_INSERT random elements in the filter */
    cnt = 0;
    while(cnt < MAX_INSERT){
        r = rand() % MAX_RANGE;

        if(!added[r]){ 
            // printf("%d ",r);
            // fflush(stdout);
            ret = cuckoo_filter_add(cf,&r,sizeof(r));
            if(ret != CUCKOO_FILTER_OK) printf("failed!\n");
            added[r] = 1;
            cnt += 1;
        }
    }

    /* Test false-positive rate*/
    int fpos, tpos, fneg, tneg, nlookups;

    for(i = 0 ; i < MAX_RUNS ; i++){
        fpos = tpos = fneg = tneg = nlookups = 0;

        for(j = 0 ; j < MAX_RANGE ; j++){
            res = cuckoo_filter_contains(cf,&j,sizeof(j));
            nlookups++;

            if(res == CUCKOO_FILTER_OK){
                if(added[j])
                    tpos++;
                else
                    fpos++;
            }else{
                if(added[j])
                    fneg++;
                else
                    tneg++;
            }

            // if(added[j]){ 
            //     if(*res == 0) // True positive
            //         tpos += 1;
            //     else // False negative, should never happen
            //         fneg += 1; 
            // }else{
            //     if(*res == 0) // False positive
            //         fpos += 1; 
            //     else // True negative
            //         tneg += 1;
            // }
        }

        printf("================================================\n");
        printf("\t\t   Results\n");
        printf("================================================\n");
        printf("Type\t\t\tExpected\tObtained\n");
        printf("False negatives\t\t%d\t\t%d\n",0,fneg);
        printf("False positives\t\t%d\t\t%d\n",(int)(0.001*MAX_INSERT),fpos);
        printf("True negatives\t\t%d\t\t%d\n",MAX_RANGE-MAX_INSERT,tneg);
        printf("True positives\t\t%d\t\t%d\n\n",MAX_INSERT,tpos);
        printf("================================================\n");
    }

    clock_t t;
    t = clock();
    cuckoo_filter_store_and_clean(cf,"cf");
    t = clock() - t;
    double time_taken = ((double)t)/CLOCKS_PER_SEC;

    printf("Time taken to save filter: %f\n",time_taken);

    res = cuckoo_filter_load(&loaded_cf,"cf_0.cuckoo");
    if(res == CUCKOO_FILTER_NOT_FOUND)
        printf("Failed to load filter\n");
    if(!memcmp(cf,loaded_cf,cuckoo_filter_memsize(cf))){
        printf("Loaded filter is different!!!!\n");
    }else{
        printf("\nAll is fine, filter succesfully loaded!\n");
    }

    return 0;
}