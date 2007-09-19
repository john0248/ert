#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>
#include <util.h>
#include <hash.h>
#include <lsf_jobs.h>
#include <ecl_util.h>
#include <msg.h>



enum lsf_status_enum_def {lsf_status_null , lsf_status_submitted , lsf_status_running , lsf_status_done , lsf_status_OK , lsf_status_exit , lsf_status_complete_fail};
#define STATUS_SIZE 6

struct lsf_job_struct {
  char             *base;
  char             *run_path;
  char 	  	   *restart_file;
  char             *fail_file;
  char             *OK_file;
  char             *special_cmd;
  char              tmp_file[256];
  char              submit_cmd[256];
  time_t  	    submit_time;
  time_t  	    start_time;
  time_t  	    complete_time;
  double  	    run_time_sec;
  int               lsf_base;
  int               max_resubmit;
  int               submit_count;
  int               job_nr;
  int               active;
  lsf_status_enum   status;
};


struct lsf_pool_struct {
  int  		  size;
  int  		  alloc_size;
  int             sleep_time;
  int             max_running;
  int             version_nr;
  bool            sub_exit; 

  int             *prev_total_status;
  int             *total_status;
  char            *summary_file;
  char            *tmp_path;
  char 		  *tmp_file_bjobs;
  char 		  *tmp_file_submit;
  char            *bin_path;
  char 		  *bsub_status_cmd;
  char            *submit_cmd_fmt;
  char            *queu;
  char            *request;
  lsf_job_type   **jobList;
  hash_type       *jobs;
  hash_type       *status_tr;
};


/*****************************************************************/


static void lsf_job_set_status(lsf_job_type *lsf_job , lsf_status_enum status) {
  lsf_job->status = status;
}


static lsf_status_enum lsf_job_get_status(const lsf_job_type *lsf_job) {
  return lsf_job->status;
}


static void lsf_job_inc_submit_count(lsf_job_type *lsf_job) {
  lsf_job->submit_count++;
}



static void lsf_job_set_ctime(const char *file , time_t *ct) {
  struct stat buffer;
  int fildes;
  
  fildes = open(file , O_RDONLY);
  fstat(fildes, &buffer);
  *ct = buffer.st_mtime;
  close(fildes);
}

static void sprintf_timestring(char *time_str , const time_t *t) {
  struct tm tr;
  localtime_r(t , &tr);
  sprintf(time_str , "%02d:%02d:%02d" , tr.tm_hour , tr.tm_min , tr.tm_sec);
}

static void lsf_job_set_submit_time(lsf_job_type *lsf_job) {
  lsf_job->submit_time = time(NULL);
  lsf_job->start_time  = time(NULL); /* In case the real start is missed */
}

static void lsf_job_set_start_time(lsf_job_type *lsf_job) {
  lsf_job->start_time = time(NULL);
}

static void lsf_job_set_complete_time(lsf_job_type *lsf_job) {
  lsf_job_set_ctime(lsf_job->restart_file , &lsf_job->complete_time);
}

static void lsf_pool_exit_job(const lsf_pool_type *lsf_pool , int ijob , lsf_status_enum status) {
  lsf_job_type *job = lsf_pool->jobList[ijob];
  
  FILE *stream = fopen(job->fail_file , "w");
  if (status == lsf_status_exit) 
    fprintf(stream, "Job:%s/LSF:%d failed with EXIT status from the LSF que system.\n" , job->base , job->lsf_base);
  else if (status == lsf_status_complete_fail)
    fprintf(stream , "Job:%s failed to produce resultfile:%s  after %d attempts - giving up.\n",job->base , job->restart_file , job->max_resubmit);
  else {
    fprintf(stderr,"%s: internal programming error. Function reached with status != (lsf_status_exit, lsf_status_complete_fail) \n",__func__);
    abort();
  }
    
  fclose(stream);
}



static void lsf_job_fprintf_status(lsf_job_type *lsf_job ,  FILE *stream) {
  const lsf_status_enum status = lsf_job_get_status(lsf_job);
  char run_time[64] , job_status[128];
  char submit_time[9],start_time[9],complete_time[9];
  char run_indicator = ' ';
  
  sprintf(submit_time   , " ");
  sprintf(start_time    , " ");
  sprintf(complete_time , " ");
  sprintf(run_time      , " ");

  switch(status) {
  case(lsf_status_null):
    sprintf(job_status,"Waiting");
    break;
  case(lsf_status_submitted):
    sprintf(job_status , "Submitted");
    break;
  case(lsf_status_running):
    sprintf(job_status , "Running");
    break;
  case(lsf_status_OK):
    sprintf(job_status , "Complete:OK");
    break;
  case(lsf_status_exit):
    sprintf(job_status , "EXITED");
    break;
  case(lsf_status_complete_fail):
    sprintf(job_status , "Failed");
    break;
  default:
    fprintf(stderr,"%s: unexpected enum value - aborting \n",__func__);
    abort();
  }
      

  if (status >= lsf_status_submitted)
    sprintf_timestring(submit_time , &lsf_job->submit_time);

  if (status >= lsf_status_running) {
    sprintf_timestring(start_time  , &lsf_job->start_time);
    if (status == lsf_status_running) {
      sprintf(run_time , "%8.0f sec"   , difftime(time(NULL) , lsf_job->start_time));
      run_indicator = 'R';
    }
  }
  
  if (status >= lsf_status_OK) {
    sprintf_timestring(complete_time , &lsf_job->complete_time);
    sprintf(run_time , "%8.0f sec"   , difftime(lsf_job->complete_time , lsf_job->start_time));
  }
  
  fprintf(stream , "%-20s  %-16s  %8s       %8s          %8s   %14s %c\n",lsf_job->base , job_status , submit_time , start_time , complete_time , run_time , run_indicator);
}






lsf_job_type * lsf_job_alloc(int job_nr , const char *base , const char *run_path ,  const char *restart_file, const char *OK_file , const char *fail_file , const char *tmp_path, int max_resubmit) {
  /*
    const char *submit_cmd = "@eclipse < eclipse.in  2> /dev/null | grep \"Job <\" | cut -f2 -d\"<\" | cut -f1 -d\">\" > ";
  */
  lsf_job_type *lsf_job = malloc(sizeof *lsf_job);
  
  if (base == NULL)
    lsf_job->base = util_alloc_string_copy(run_path);
  else
    lsf_job->base = util_alloc_string_copy(base);
  lsf_job->job_nr        = job_nr;
  lsf_job->run_path      = util_alloc_string_copy(run_path);
  lsf_job->restart_file  = util_alloc_string_copy(restart_file);
  lsf_job->fail_file     = util_alloc_string_copy(fail_file);
  lsf_job->OK_file       = util_alloc_string_copy(OK_file);
  lsf_job->special_cmd   = NULL;
  /*
    lsf_job->submit_cmd    = malloc(strlen(run_path) + 7 + strlen(submit_cmd));
    sprintf(lsf_job->submit_cmd , "cd %s ; %s" , lsf_job->run_path , submit_cmd);
  */
  lsf_job->max_resubmit   = max_resubmit;
  lsf_job->submit_count   = 0;

  util_unlink_existing(lsf_job->fail_file);
  util_unlink_existing(lsf_job->OK_file);
  
  lsf_job_set_status(lsf_job , lsf_status_null);
  lsf_job->active = false;
  return lsf_job;
}


static void lsf_job_activate(lsf_job_type * job) {
  job->active = true;
}


void lsf_job_free(lsf_job_type *lsf_job) {
  /*free(lsf_job->submit_cmd);*/
  free(lsf_job->run_path);
  free(lsf_job->base);
  free(lsf_job->fail_file);
  free(lsf_job->OK_file);
  if (lsf_job->special_cmd != NULL) free(lsf_job->special_cmd);
  free(lsf_job);
}



/*
  If an eclipse job has failed, it will typically leave a broken
  *.SMSPEC file around, and this will lead to the failure of all 
  subsequent restarts in the same directory.
*/
static void lsf_job_unlink_smspec(const lsf_job_type *lsf_job) {
  char *file;

  file = ecl_util_alloc_filename(lsf_job->run_path , lsf_job->base , ecl_summary_header_file , true , -1);
  if (util_file_exists(file)) unlink(file);
  free(file);
  
  file = ecl_util_alloc_filename(lsf_job->run_path , lsf_job->base , ecl_summary_header_file , false , -1);
  if (util_file_exists(file)) unlink(file);
  free(file);
}




/*  
    This function is custom made to parse a file like this: 

    Job <874450> is submitted to queue <common>.

    and return the jobid as an integer.
*/

static int lsf_job_parse_bsub_stdout(const char * stdout_file) {
  int jobid;
  FILE * stream = util_fopen(stdout_file , "r");
  {
    char buffer[16];
    int c;
    int i;
    do {
      c = fgetc(stream);
    } while (c != '<');

    i = -1;
    do {
      i++;
      buffer[i] = fgetc(stream);
    } while(buffer[i] != '>');
    buffer[i] = '\0';
    jobid = atoi(buffer);
  }
  fclose(stream);
  return jobid;
}



static int lsf_job_submit(lsf_job_type *lsf_job , const char * submit_cmd_fmt ,  const char * bin_path , const char * tmp_path ) {
  if (!util_file_exists(lsf_job->run_path)) {
    fprintf(stderr,"%s: fatal error when submitting job:%s - run_path:%s does not exist \n",__func__ , lsf_job->base , lsf_job->run_path);
    abort();
  }
  {
    int job_id;
    sprintf(lsf_job->tmp_file , "%s/enkf-submit-%08d-%d" , tmp_path , getpid() , lsf_job->job_nr);
    sprintf(lsf_job->submit_cmd , submit_cmd_fmt , lsf_job->run_path , lsf_job->base , lsf_job->base , lsf_job->run_path , lsf_job->base , lsf_job->tmp_file);
    system(lsf_job->submit_cmd);
    
    job_id = lsf_job_parse_bsub_stdout(lsf_job->tmp_file);
    lsf_job->lsf_base = job_id;
    util_unlink_existing(lsf_job->tmp_file); 
    return job_id;
  }
}
	    
 /*  const char *tmp_file_static = "bsub-"; */
/*   char       *tmp_file = malloc(strlen(tmp_file_static) + 7); */
/*   sprintf(tmp_file , "%s%06d" , tmp_file_static , getpid() % 1000000); */
/*   { */
/*     /\* The same storage is used for both the variables cmd and tmp_full_path ... *\/ */
/*     char *cmd           = malloc(strlen(lsf_job->submit_cmd) + strlen(tmp_path) + strlen(tmp_file) + 3); */
/*     char *tmp_full_path = cmd; */
/*     lsf_job_unlink_smspec(lsf_job); */
/*     sprintf(cmd , "%s %s/%s" , lsf_job->submit_cmd , tmp_path , tmp_file); */
/*     system(cmd); */
/*     sprintf(tmp_full_path , "%s/%s" , tmp_path , tmp_file); */
/*     { */
/*       FILE *stream = fopen(tmp_full_path , "r"); */
/*       int read1; */
/*       read1 = fscanf(stream , "%d" , &lsf_job->lsf_base); */
/*       fclose(stream); */
/*       if (read1 == 1)  */
/* 	/\* */
/* 	  Submit status is handled in the pool object. */
/* 	*\/ */
/* 	time(&lsf_job->submit_time); */
/*       else { */
/* 	fprintf(stderr,"*** Submitting job:%s failed - could not get LSF base \n",lsf_job->base); */
/* 	fprintf(stderr,"*** Maybe you are trying to run from a machine without \n*** the que system installed?\n"); */
/* 	abort(); */
/*       } */
/*     } */
/*     util_unlink_existing(tmp_full_path); */
/*   } */
/*   free(tmp_file); */
/*   lsf_job->submit_count++; */
/*   return lsf_job->lsf_base; */
/* } */



static bool lsf_job_can_reschedule(lsf_job_type *lsf_job) {
  if (lsf_job->submit_count <= lsf_job->max_resubmit) {
    return true;
  } else 
    return false;
}



bool lsf_job_complete_OK(lsf_job_type *lsf_job) {
  if (util_file_exists(lsf_job->restart_file)) {
    FILE *stream;
    struct stat buffer;
    int fildes;
    
    fildes = open(lsf_job->restart_file , O_RDONLY);
    fstat(fildes , &buffer);
    close(fildes);

    if (buffer.st_size > 0) {
      lsf_job->complete_time = buffer.st_mtime;

      stream = fopen(lsf_job->OK_file , "w");
      fprintf(stream , "Job: %s completed successfully \n",lsf_job->base);
      fclose(stream);
      return true;

    } else
      /* Zero size */
      return false;
  }  else
    return false;
}


/*****************************************************************/


lsf_pool_type * lsf_pool_alloc(int sleep_time , int max_running , bool sub_exit, int version_nr , const char * queu , const char * request, 
			       const char * summary_path , const char * summary_file , const char *bsub_status_cmd , const char * bin_path , 
			       const char *tmp_path) {
  const char *tmp_file = "bjobs.jobList";
  lsf_pool_type *lsf_pool = malloc(sizeof *lsf_pool);
  lsf_pool->alloc_size = 100;
  lsf_pool->jobList    = calloc(lsf_pool->alloc_size    , sizeof *lsf_pool->jobList);
  
  if (summary_file != NULL && summary_path != NULL) {
    lsf_pool->summary_file = util_alloc_full_path(summary_path , summary_file);
    util_make_path(summary_path);
  } else
    lsf_pool->summary_file = NULL;
  
  lsf_pool->bin_path 	   = util_alloc_string_copy(bin_path);
  lsf_pool->tmp_path 	   = util_alloc_string_copy(tmp_path);
  lsf_pool->request  	   = util_alloc_string_copy(request);
  lsf_pool->queu     	   = util_alloc_string_copy(queu);
  lsf_pool->tmp_file_bjobs = malloc(strlen(tmp_path) + strlen(tmp_file) + 8);
  sprintf(lsf_pool->tmp_file_bjobs , "%s/%s.%d" , tmp_path , tmp_file , (getpid() % 1000000));

  lsf_pool->bsub_status_cmd = malloc(strlen(bsub_status_cmd) + 4 + strlen(lsf_pool->tmp_file_bjobs));
  sprintf(lsf_pool->bsub_status_cmd , "%s > %s" , bsub_status_cmd , lsf_pool->tmp_file_bjobs);
  
  lsf_pool->jobs      = hash_alloc(2*lsf_pool->alloc_size);
  lsf_pool->status_tr = hash_alloc(10);

  hash_insert_int(lsf_pool->status_tr , "PEND"   , lsf_status_submitted);
  hash_insert_int(lsf_pool->status_tr , "PSUSP"  , lsf_status_submitted);
  hash_insert_int(lsf_pool->status_tr , "RUN"    , lsf_status_running);
  hash_insert_int(lsf_pool->status_tr , "SSUSP"  , lsf_status_running);
  hash_insert_int(lsf_pool->status_tr , "USUSP"  , lsf_status_running);
  hash_insert_int(lsf_pool->status_tr , "EXIT"   , lsf_status_exit);
  hash_insert_int(lsf_pool->status_tr , "DONE"   , lsf_status_done);
  hash_insert_int(lsf_pool->status_tr , "UNKWN"  , lsf_status_exit); /* Uncertain about this one */
  
  lsf_pool->sleep_time     = sleep_time;
  lsf_pool->max_running    = max_running;
  lsf_pool->sub_exit       = sub_exit;
  lsf_pool->total_status   = calloc(STATUS_SIZE , sizeof *lsf_pool->total_status);
  lsf_pool->prev_total_status = calloc(STATUS_SIZE , sizeof *lsf_pool->total_status);
  lsf_pool->submit_cmd_fmt = malloc(256);
  lsf_pool->version_nr     = version_nr;
  sprintf(lsf_pool->submit_cmd_fmt , "bsub -o %s/%s.stdout -q %s -J %s -R\"%s\" %s/ecl_submit.x %s %s %d > %s" , 
	  "%s"                 ,
	  "%s"                 , /* lsf stdout file */
	  lsf_pool->queu       , 
	  "%s"                 , /* lsf job name   */
	  lsf_pool->request    , 
	  lsf_pool->bin_path   , 
	  "%s"                 , /* Run path       */
	  "%s"                 , /* Base name      */
	  lsf_pool->version_nr , 
	  "%s"                 ); /* tmp file       */
  
  lsf_pool->size = 0;
  return lsf_pool;
}


static lsf_status_enum lsf_pool_iget_status(const lsf_pool_type *lsf_pool , int ijob) {
  return lsf_job_get_status(lsf_pool->jobList[ijob]);
}



static void lsf_pool_iset_status(const lsf_pool_type *lsf_pool , int ijob , lsf_status_enum new_status) {
  const lsf_status_enum old_status = lsf_pool_iget_status(lsf_pool , ijob);
  
  if (old_status != lsf_status_OK && old_status != lsf_status_complete_fail) {
    if (old_status != new_status) {
      switch(new_status) {
      case(lsf_status_submitted):
	lsf_job_set_submit_time(lsf_pool->jobList[ijob]);
	break;
      case(lsf_status_running):
	lsf_job_set_start_time(lsf_pool->jobList[ijob]);
	break;
      case(lsf_status_done):
	lsf_job_set_complete_time(lsf_pool->jobList[ijob]);
	break;
      case(lsf_status_exit):
	/*printf("job:%d  is exited ... \n",lsf_pool->jobList[ijob]->lsf_base);*/
	/*lsf_pool_exit_job(lsf_pool , ijob , lsf_status_exit);*/
	break;
      case(lsf_status_complete_fail):
	lsf_pool_exit_job(lsf_pool , ijob , lsf_status_complete_fail);
	break;
      default:
	/* No op */
	break;
      }
    }
    lsf_job_set_status(lsf_pool->jobList[ijob] , new_status);

    lsf_pool->total_status[old_status]--;
    lsf_pool->total_status[new_status]++;
  }
}


static void lsf_pool_isubmit(lsf_pool_type *lsf_pool , int ijob) {
  if (ijob >= lsf_pool->size) {
    fprintf(stderr,"%s: trying to submit job:%d non-existing job - aborting \n",__func__ , ijob);
    abort();
  }
  if (lsf_pool->jobList[ijob]->active) {
    {
      char char_base[16];
      int new_base = lsf_job_submit(lsf_pool->jobList[ijob] , lsf_pool->submit_cmd_fmt ,  lsf_pool->bin_path , lsf_pool->tmp_path);
      sprintf(char_base , "%d" , new_base);
      hash_insert_int(lsf_pool->jobs , char_base , ijob);
    }
    lsf_pool_iset_status(lsf_pool , ijob , lsf_status_submitted);
    lsf_job_inc_submit_count(lsf_pool->jobList[ijob]);
  } 
}


static void lsf_pool_fprintf_summary(const lsf_pool_type *lsf_pool) {
  int job;
  FILE *stream = fopen(lsf_pool->summary_file , "w");
  
  
  if (stream != NULL) {
    fprintf(stream , "Job                   Status           submit-time    start-time      complete-time       run-time\n");
    fprintf(stream , "----------------------------------------------------------------------------------------------------\n");
    for (job = 0; job < lsf_pool->size; job++) 
      lsf_job_fprintf_status(lsf_pool->jobList[job] , stream);
    fclose(stream);
  } else {
    fprintf(stderr,"%s: failed to open summary file:%s - aborting \n",__func__ , lsf_pool->summary_file);
    abort();
  }
}




int lsf_pool_get_active(const lsf_pool_type *lsf_pool) {
  return lsf_pool->total_status[lsf_status_submitted] + lsf_pool->total_status[lsf_status_running];
}


void lsf_pool_iactivate_job(lsf_pool_type * lsf_pool , int ijob) {
  lsf_job_activate(lsf_pool->jobList[ijob]);
}


void lsf_pool_add_job(lsf_pool_type *lsf_pool , const char *base , const char *run_path , const char *restart_file, const char *OK_file, const char *fail_file , int max_resubmit) {
  lsf_job_type *new_job = lsf_job_alloc(lsf_pool->size , base , run_path , restart_file , OK_file , fail_file , lsf_pool->tmp_path , max_resubmit);

  if (lsf_pool->size == lsf_pool->alloc_size) {
    lsf_pool->alloc_size *= 2;
    lsf_pool->jobList = realloc(lsf_pool->jobList , lsf_pool->alloc_size * sizeof *lsf_pool->jobList);
  }
  lsf_pool->jobList[lsf_pool->size] = new_job;
  lsf_pool->size++;

  /* 
     Dette er eneste punkt hvor netto i total_status endres.
  */
  lsf_pool_iactivate_job(lsf_pool , lsf_pool->size - 1);
  lsf_pool->total_status[lsf_status_null]++;
  if (lsf_pool_get_active(lsf_pool) < lsf_pool->max_running || lsf_pool->sub_exit)
    lsf_pool_isubmit(lsf_pool , lsf_pool->size - 1);
}



static void lsf_pool_delete_job(lsf_pool_type * lsf_pool , int ijob) {
  int old_base = lsf_pool->jobList[ijob]->lsf_base;
  char old_base_char[16];
  sprintf(old_base_char , "%d" , old_base);
  if (hash_has_key(lsf_pool->jobs , old_base_char)) {
    hash_del(lsf_pool->jobs , old_base_char); /* We orphan the job which has completed */
  }
  else 
    fprintf(stderr,"%s: Job:%s does not exist - internal ERROR \n",__func__ , old_base_char);
}

/* 
   A problem is that the jobs with status EXIT are
   left in the system for many iterations, but must
   of course only be resubmitted once. 
*/

static bool lsf_pool_ireschedule(lsf_pool_type *lsf_pool , int ijob) {
  if (lsf_job_can_reschedule(lsf_pool->jobList[ijob])) {
    lsf_pool_delete_job(lsf_pool , ijob);
    lsf_pool_iset_status(lsf_pool , ijob , lsf_status_null);
    lsf_job_unlink_smspec(lsf_pool->jobList[ijob]);
    return true;
  } else {
    if (lsf_pool_iget_status(lsf_pool , ijob) == lsf_status_done) {
      lsf_pool_delete_job(lsf_pool , ijob);
      lsf_pool_iset_status(lsf_pool , ijob , lsf_status_complete_fail);
    }
    return false;
  }
}



static bool lsf_pool_complete_OK(const lsf_pool_type *lsf_pool , int ijob) {
  return lsf_job_complete_OK(lsf_pool->jobList[ijob]);
}



static void lsf_pool_update_status(lsf_pool_type *lsf_pool) {
  {
    int attempt = 0;
    do {
      system(lsf_pool->bsub_status_cmd);
      attempt++;
      if (!util_file_exists(lsf_pool->tmp_file_bjobs))
	sleep(1);
    } while (attempt < 10 && !util_file_exists(lsf_pool->tmp_file_bjobs));

    if (!util_file_exists(lsf_pool->tmp_file_bjobs)) {
      fprintf(stderr,"%s: failed to find status file:%s ... aborting \n", __func__ , lsf_pool->tmp_file_bjobs);
      abort();
    }
  }
  {
    const char newline = '\n';
    bool cont = true;
    int  jobbase_int;
    char jobbase[16];
    char user[32];
    char status[16];
    FILE *stream = fopen(lsf_pool->tmp_file_bjobs , "r");;
    char c;
    int read;
    
    do {
      c = fgetc(stream);
    } while (c != newline && c != EOF);
    
    if (c != EOF) {
      do {
	read = fscanf(stream , "%d %s %s",&jobbase_int , user , status);
	if (read == 3) {
	  sprintf(jobbase,"%d" , jobbase_int);
	  do {
	    c = fgetc(stream);
	  } while (c != newline && c != EOF);
	  if (c == EOF) cont = false;
	  if (hash_has_key(lsf_pool->jobs , jobbase)) {
	    int job_nr = hash_get_int(lsf_pool->jobs , jobbase);
	    lsf_pool_iset_status(lsf_pool , job_nr , hash_get_int(lsf_pool->status_tr , status));
	  } 
	} else if (read == 0) {
	  do {
	    c = fgetc(stream);
	  } while (c != newline && c != EOF);
	  if (c == EOF) cont = false;
	}
	
	if (cont) {
	  c = fgetc(stream);
	  if (c == EOF) 
	    cont = false;
	  else
	    ungetc(c , stream);
	}
      } while (cont);
    }
    fclose(stream);
    unlink(lsf_pool->tmp_file_bjobs);
  } 
}





int lsf_pool_run_jobs(lsf_pool_type *lsf_pool) {
  if (lsf_pool->sub_exit) 
    /*
      In this case there is no job control ...
    */
    return 0;
  else {
    char spinner[4];
    bool cont;
    bool redraw = true;
    int ijob , phase;
    phase = 0;

    spinner[0] = '-';
    spinner[1] = '\\';
    spinner[2] = '|';
    spinner[3] = '/';

    do {
      memcpy(lsf_pool->prev_total_status , lsf_pool->total_status , sizeof * lsf_pool->total_status * STATUS_SIZE);
      cont = true;
      /* 
	 First step: submitting basel(??) jobs 
      */
      if (lsf_pool_get_active(lsf_pool) < lsf_pool->max_running) {
	ijob = 0;
	do {
	  if (lsf_pool_iget_status(lsf_pool , ijob) == lsf_status_null)
	    lsf_pool_isubmit(lsf_pool , ijob);
	  ijob++;
	} while (lsf_pool_get_active(lsf_pool) < lsf_pool->max_running && ijob < lsf_pool->size);
      }
      /*
	Second step: update status
      */
      lsf_pool_update_status(lsf_pool);
      /*
	Third step: check complete/EXIT jobs.
      */
     
      /*
	Will reschedule jobs with EXIT status as well - they sometimes just fail to start ... 
      */
      for (ijob = 0; ijob < lsf_pool->size; ijob++) {
	if (lsf_pool_iget_status(lsf_pool , ijob) == lsf_status_exit) {
	  redraw = true;
	  if (lsf_pool_ireschedule(lsf_pool , ijob))
	    printf("\b Job:%d returned with EXIT status - resubmitting [Attempt:%d] \n",ijob+1 ,  lsf_pool->jobList[ijob]->submit_count);
	  else
	    printf("\b Job:%d returned with EXIT status - no more resubmits \n",ijob+1);
	}
      }

      
      for (ijob = 0; ijob < lsf_pool->size; ijob++) {
	if (lsf_pool_iget_status(lsf_pool , ijob) == lsf_status_done) {
	  if (lsf_pool_complete_OK(lsf_pool , ijob)) { 
	    lsf_pool_iset_status(lsf_pool , ijob , lsf_status_OK);
	  } else {
	    redraw = true;
	    printf("\b Could not find result_file: %s ",lsf_pool->jobList[ijob]->restart_file);
	    if (lsf_pool_ireschedule(lsf_pool , ijob))
	      printf("rescheduling: %s [Attempt:%d] \n",lsf_pool->jobList[ijob]->base , lsf_pool->jobList[ijob]->submit_count);
	    else
	      printf("no more attempts - job failed hard - about to exit.\n");
	  }
	}
      }
      

      if (lsf_pool->total_status[lsf_status_OK] + lsf_pool->total_status[lsf_status_exit] == lsf_pool->size)
	cont = false;
      {
	int i;

	for (i=0; i < STATUS_SIZE; i++)
	  if (lsf_pool->total_status[i] != lsf_pool->prev_total_status[i])
	    redraw = true;

	if (!redraw) {
	  phase = (phase + 1) % 4;
	  printf("\b%c",spinner[phase]);	  
	  fflush(stdout);
	} else {
	  struct tm ts;
	  time_t t;  
	  time(&t);
	  localtime_r(&t , &ts);
	  printf("\b \ntotal: %2d %2d %2d | %2d %2d %2d  | Last update: %02d:%02d:%02d :  ",lsf_pool->total_status[0] , lsf_pool->total_status[1] , lsf_pool->total_status[2] , lsf_pool->total_status[3],
		 lsf_pool->total_status[4] , lsf_pool->total_status[5] , ts.tm_hour, ts.tm_min , ts.tm_sec);  
	  fflush(stdout);
	}
      }


      {
	int total_jobs = 0;
	for (ijob = 0; ijob < 6; ijob++)
	  total_jobs += lsf_pool->total_status[ijob];
	if (total_jobs != lsf_pool->size) {
	  fprintf(stderr,"%s: Internal error: total number of jobs has changed %d -> %d - aborting.\n",__func__ , lsf_pool->size , total_jobs);
	  abort();
	}
      }
    
      if (lsf_pool->summary_file != NULL) 
	lsf_pool_fprintf_summary(lsf_pool);

      if (cont)
	sleep(lsf_pool->sleep_time);
      
      redraw = false;
    } while (cont);
    /*
      Print warning about failed jobs.
    */
    if (lsf_pool->total_status[lsf_status_exit] >= 0) {
      for (ijob = 0; ijob < lsf_pool->size; ijob++) {
	if (lsf_pool_iget_status(lsf_pool , ijob) == lsf_status_exit)
	  printf("Job : %04d / %s failed \n",ijob + 1,lsf_pool->jobList[ijob]->base);
      }
    }
    return lsf_pool->total_status[lsf_status_exit];
  }
  }


void lsf_pool_set_fail_vector(const lsf_pool_type * lsf_pool , int *fail_vector) {
  int ijob;
  int ifail = 0;
  for (ijob = 0; ijob < lsf_pool->size; ijob++) {
    if (lsf_pool_iget_status(lsf_pool , ijob) == lsf_status_exit) {
      fail_vector[ifail] = ijob;
      ijob++;
    }
  }
}



void lsf_pool_free(lsf_pool_type *lsf_pool) {
  free(lsf_pool->tmp_path);
  free(lsf_pool->tmp_file_bjobs);
  free(lsf_pool->bsub_status_cmd);
  free(lsf_pool->bin_path);
  free(lsf_pool->submit_cmd_fmt);
  free(lsf_pool->queu);
  free(lsf_pool->request);
  hash_free(lsf_pool->jobs);
  hash_free(lsf_pool->status_tr);
  {
    int i;
    for (i=0; i < lsf_pool->size; i++)
      lsf_job_free(lsf_pool->jobList[i]);
    free(lsf_pool->jobList);
  }
  if (lsf_pool->summary_file != NULL)
    free(lsf_pool->summary_file);
  free(lsf_pool->total_status);
  free(lsf_pool->prev_total_status);
  free(lsf_pool);
}
