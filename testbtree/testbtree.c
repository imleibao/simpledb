#include"btree.h"
#include<assert.h>
//stdno:int stdname:char[20] stdage:int stdgpa:float
typedef struct std{
  int stdNo;
  char stdName[20];
  int age;
  float gpa;
} Std;
int  main(){
  Btree ** ppBtree;
  BtCursor * btc;
  sqliteBtreeOpen("testbtree.db", 1024, ppBtree);
  int firstt;

  sqliteBtreeBeginTrans(*ppBtree);
  sqliteBtreeCreateTable(*ppBtree, &firstt);
  
  Std std1;
  std1.gpa = 1.4;
  std1.age = 23;
  std1.stdNo = 1;
  char a[9]="xiaoming";
  memcpy(std1.stdName, &a, 9) ;
  sqliteBtreeCursor(*ppBtree, firstt, &btc);
  int i = 0;
  for(i = 0; i < 20 ; ++i){
    std1.stdNo += i;
    std1.age += i;
    sqliteBtreeInsert(btc, (void*)&std1.stdNo, sizeof(int), (void*)&std1, sizeof(std1));
  }

  sqliteBtreeCommit(*ppBtree);
  
  Std std2;
  std2.stdNo = 4;
  int rec;
  sqliteBtreeMoveto(btc,(void*) &std2.stdNo, sizeof(int), &rec);
  assert(rec == 0);
  sqliteBtreeData(btc,0 ,sizeof(Std), (char*)&std2);
}




















