#include"pager.h"

#include<stdio.h>
int main(){
  Pager *pPager;
  printf("ss");
   sqlitepager_open(&pPager, "test.db", 6,0,0);
  void *ppPage1, *ppPage2, *ppPage3, *ppPage4, *ppPage5, *ppPage6, *ppPage7;
   sqlitepager_get(pPager, 1, &ppPage1);
   sqlitepager_get(pPager, 2, &ppPage2);
  sqlitepager_get(pPager, 3, &ppPage3);
	sqlitepager_get(pPager, 4, &ppPage4);
	sqlitepager_get(pPager, 5, &ppPage5);
	sqlitepager_get(pPager, 6, &ppPage6);
	sqlitepager_get(pPager, 7, &ppPage7);
  printf("%p \t %p \t %p\n", ppPage1, ppPage2, ppPage3);

  sqlitepager_begin(ppPage1);
  memset(ppPage1,1,1024);
  sqlitepager_write(ppPage1);
  sqlitepager_commit(pPager);

  sqlitepager_begin(ppPage2);
  memset(ppPage2,1,1024);
  sqlitepager_write(ppPage2);
  sqlitepager_commit(pPager);

  sqlitepager_close(pPager);
   return 0;
}
