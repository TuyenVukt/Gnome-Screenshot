
#include <string.h>
#include <stdio.h>

int main()
{
   char str[100] = "/home/vuductuyen/Pictures/Screenshot from 2021-12-17 11-02-22.png";
   char name[50];
   int i;
   int pLast;
   int k = 0;
   int len = strlen(str);
   for (i = 0; i < strlen(str); i++)
   {
      if (str[i] == '/')
      {
         pLast = i;
         printf("i = %d\n", pLast);
      };
   }
   name[k++] = '\'';
   for (i = pLast + 1; i < strlen(str); i++)
      name[k++] = str[i];
   name[k++] = '\'';
   name[k] = '\0';
   printf("->name: %s\n", name);
   k = 0;

   for (i = pLast + 1; i < len + 2; i++)
   {
      str[i] = name[k++];
   }
   str[++i] = '\'';
   str[++i] = '\0';
   printf("->path: %s\n", str);
   char command[120];
    strcpy(command,"mypaint ");
    strcat(command, str);
    printf("->command: %s\n", command);
  
}