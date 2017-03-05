#include"pager.h"
void f(void **data){*data = 0;}
#include<stdio.h>
int main(){
  Pager *pPager;
  printf("ss");
   mndbpager_open(&pPager, "test.db", 6);
   void *ppPage1, *ppPage2, *ppPage3,*ppPage31, *ppPage4,*ppPage5,*ppPage6,*ppPage7;
   void *test;
 unsigned char a = 1;
   test = malloc(1);
   memcpy(test,&a ,1);
   mndbpager_get(pPager, 1, &ppPage1);
   mndbpager_get(pPager, 2, &ppPage2);
  mndbpager_get(pPager, 3, &ppPage3);
  mndbpager_get(pPager, 3, &ppPage31);
  mndbpager_get(pPager, 4, &ppPage4);
  mndbpager_get(pPager, 5, &ppPage5);
  mndbpager_get(pPager, 6, &ppPage6);

  mndbpager_unref(ppPage2);
  mndbpager_get(pPager, 7,&ppPage7);

  printf("%p \t %p \t %p\n", ppPage1, ppPage2, ppPage3,ppPage4);

 
  mndbpager_begin(ppPage3);
  memcpy(ppPage3,&a,1024);
  mndbpager_write(ppPage3);
  mndbpager_commit(pPager);

  mndbpager_begin(ppPage2);
  memcpy(ppPage2,&a,1024);
  mndbpager_write(ppPage2);
  mndbpager_commit(pPager);

  mndbpager_begin(ppPage1); 
  memset(ppPage1,&a,1024);
  mndbpager_write(ppPage1);
  mndbpager_commit(pPager); 
  mndbpager_close(pPager);
   return 0;
}
