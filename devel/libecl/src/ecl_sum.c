#include <string.h>
#include <stdbool.h>
#include <ecl_kw.h>
#include <ecl_block.h>
#include <math.h>
#include <ecl_fstate.h>
#include <ecl_sum.h>
#include <hash.h>
#include <util.h>
#include <time.h>
#include <set.h>
#include <util.h>

#define ECL_DUMMY_WELL ":+:+:+:+"
/** 

ECLIPSE vector naming conventions - initial letter:

 G : Group data
 R : Region data 
 W : Well data
 F : Field data
 ....

*/





struct ecl_sum_struct {
  ecl_fstate_type * header;
  ecl_fstate_type * data;
  hash_type       * index_hash;
  hash_type       * kw_index_hash;
  hash_type       * unit_hash;
  hash_type       * field_index;
  int               fmt_mode;
  int               Nwells , Nvars , param_offset;
  char            **well_list;
  char            * base_name;
  bool              endian_convert;
  bool              unified;
  time_t            sim_start_time;
};




static ecl_sum_type * ecl_sum_alloc_empty(int fmt_mode , bool endian_convert) {
  ecl_sum_type *ecl_sum;
  ecl_sum = malloc(sizeof *ecl_sum);
  ecl_sum->fmt_mode       = fmt_mode;
  ecl_sum->endian_convert = endian_convert;
  ecl_sum->unified        = true;  /* Dummy */
  ecl_sum->index_hash     = hash_alloc(10);
  ecl_sum->kw_index_hash  = hash_alloc(10);
  ecl_sum->unit_hash      = hash_alloc(10);
  ecl_sum->header         = NULL;
  ecl_sum->data           = NULL;
  ecl_sum->well_list      = NULL;
  ecl_sum->base_name      = NULL;
  ecl_sum->sim_start_time = -1;
  return ecl_sum;
}



void ecl_sum_fread_alloc_data(ecl_sum_type * sum , int files , const char **data_files , bool report_mode) {
  if (files <= 0) {
    fprintf(stderr,"%s: number of data files = %d - aborting \n",__func__ , files);
    abort();
  }
  {
    ecl_file_type file_type;
    bool fmt_file;
    int report_nr;
    
    ecl_util_get_file_type(data_files[0] , &file_type , &fmt_file , &report_nr);
    sum->data  = ecl_fstate_fread_alloc(files , data_files   , file_type , report_mode , sum->endian_convert);
    /*
      Check size of PARAMS block...
      {
      ecl_block_type * data_block1 = ecl_fstate_get_block();
      }
    */
  }
}
  

/*
  Not in use - yet finding the well_vars
  variable from an existing SMSPEC file is
  painful.
*/

/* 
   Look at ecl_sum_set_well_header - which has some of the
   relevant code needed here.
*/
static void ecl_sum_init_header(ecl_sum_type * ecl_sum , int n_wells ,  const char ** wells , int total_vars  , int well_vars , const char ** vars, const char ** units ) {
  hash_type * index_hash     = hash_alloc(10);
  hash_type * var_index_hash = hash_alloc(10);
  hash_type * unit_hash;

  const int well_var_offset = total_vars - well_vars;
  int iw,ivar;
  if (units == NULL)
    unit_hash = NULL;
  else
    unit_hash = hash_alloc(10); 

  for (iw = 0; iw < n_wells; iw++) 
    hash_insert_hash_owned_ref(index_hash , wells[iw] , hash_alloc(10) , hash_free__);

  for (ivar = 0; ivar < well_vars; ivar++) {
    hash_insert_int(var_index_hash , vars[ivar] , well_var_offset + ivar);
    if (unit_hash != NULL)
      hash_insert_string(unit_hash , vars[ivar] , units[ivar]);
    for (iw = 0; iw < n_wells; iw++) {
      int index = well_var_offset + ivar * n_wells + iw;
      hash_insert_int( hash_get(index_hash , wells[iw]) , vars[ivar] , index);
    }

  }
}



static void ecl_sum_fread_header(ecl_sum_type * ecl_sum, const char * header_file) {
  ecl_sum->header         = ecl_fstate_fread_alloc(1     , &header_file , ecl_summary_header_file , false , ecl_sum->endian_convert);
  {
    char well[9] , kw[9];
    int *date;
    ecl_block_type * block = ecl_fstate_iget_block(ecl_sum->header , 0);
    ecl_kw_type *wells     = ecl_block_get_kw(block , "WGNAMES"); 
    ecl_kw_type *keywords  = ecl_block_get_kw(block , "KEYWORDS"); 
    ecl_kw_type *startdat  = ecl_block_get_kw(block , "STARTDAT");
    ecl_kw_type *units     = ecl_block_get_kw(block , "UNITS");
    int index;
    
    if (startdat == NULL) {
      fprintf(stderr,"%s could not locate STARTDAT keyword in header - aborting \n",__func__);
      abort();
    }
    date = ecl_kw_get_int_ptr(startdat);
    ecl_sum->sim_start_time = util_make_time1(date[0] , date[1] , date[2]);
    {
      /*
	Fills a kw_index_hash pointing to first occurence of a certain
	variable - not very sensible for the well variables like
	WOPR. And additionally a unit_hash: unit_hash["WOPR"] =
	"Barels/day"...
      */
	
      set_type  * kw_set    = set_alloc_empty();
      for (index=0; index < ecl_kw_get_size(keywords); index++) {
	util_set_strip_copy(kw , ecl_kw_iget_ptr(keywords , index));
	if (set_add_key(kw_set , kw)) {
	  char * unit = util_alloc_strip_copy(ecl_kw_iget_ptr(units , index));
	  hash_insert_int(ecl_sum->kw_index_hash , kw , index);
	  hash_insert_hash_owned_ref(ecl_sum->unit_hash , kw , unit , free);
	}
      }
      ecl_sum->Nvars  = set_get_size(kw_set);
      set_free(kw_set);
    }
    
    {
      set_type *well_set = set_alloc_empty();
      for (index=0; index < ecl_kw_get_size(wells); index++) {
	util_set_strip_copy(well , ecl_kw_iget_ptr(wells    , index));
	if (strlen(well) > 0 && strcmp(well , ECL_DUMMY_WELL) != 0) {
	  set_add_key(well_set , well);
	  util_set_strip_copy(kw   , ecl_kw_iget_ptr(keywords , index));
	
	  if (!hash_has_key(ecl_sum->index_hash , well)) 
	    hash_insert_hash_owned_ref(ecl_sum->index_hash , well , hash_alloc(10) , hash_free__);
	  
	  {
	    hash_type * var_hash = hash_get(ecl_sum->index_hash , well);
	    hash_insert_int(var_hash , kw , index);
	  }
	}
      }
      ecl_sum->Nwells    = set_get_size(well_set);
      ecl_sum->well_list = set_alloc_keylist(well_set);
      set_free(well_set);
    }
      
    
    /*
      Index test:
      -----------
      for (index=0; index < ecl_kw_get_size(wells); index++) {
      util_set_strip_copy(well , ecl_kw_iget_ptr(wells    , index));
      util_set_strip_copy(kw   , ecl_kw_iget_ptr(keywords , index));
      printf("%s   %s   %d -> ",well , kw,index);
      if (ecl_sum_has_well_var(ecl_sum , well , kw))
	printf("%d \n",ecl_sum_get_index(ecl_sum , well , kw));
      else
	printf("---\n");
    }
    */
  }


  /*
    This is the only place the kw_index_hash field
    is used - maybe a bit overkill?
  */
  if (hash_has_key(ecl_sum->kw_index_hash , "DAY")) {
    int iblock;
    int day_index   = hash_get_int(ecl_sum->kw_index_hash , "DAY");
    int month_index = hash_get_int(ecl_sum->kw_index_hash , "MONTH");
    int year_index  = hash_get_int(ecl_sum->kw_index_hash , "YEAR");
    
    for (iblock = 0; iblock < ecl_fstate_get_size(ecl_sum->data); iblock++) {
      ecl_block_type * block = ecl_fstate_iget_block(ecl_sum->data , iblock);
      ecl_block_set_sim_time_summary(block , /*time_index , years_index , */ day_index , month_index , year_index);
    }
  } 
}


ecl_sum_type * ecl_sum_fread_alloc(const char *header_file , int files , const char **data_files , bool report_mode , bool endian_convert) {
  ecl_sum_type *ecl_sum   = ecl_sum_alloc_empty(ECL_FMT_AUTO , endian_convert);
  ecl_sum_fread_alloc_data(ecl_sum , files , data_files , report_mode);
  ecl_sum_fread_header(ecl_sum , header_file);
  return ecl_sum;
}
	

									
static void ecl_sum_set_unified(ecl_sum_type *ecl_sum , bool unified) {
  ecl_sum->unified = unified;
  ecl_fstate_set_unified(ecl_sum->data , unified);
}



/* ecl_sum_type * ecl_sum_alloc_new(const char *base_name , int Nwells, int Nvars, int param_offset , int fmt_mode , bool report_mode , bool endian_convert , bool unified) { */
/*   ecl_sum_type *ecl_sum = ecl_sum_alloc_empty(fmt_mode , endian_convert ); */
/*   ecl_sum_set_unified(ecl_sum , unified); */
/*   ecl_sum->header       = ecl_fstate_alloc_empty(fmt_mode , ecl_summary_header_file , false , endian_convert); */
/*   if (unified) */
/*     ecl_sum->data         = ecl_fstate_alloc_empty(fmt_mode , ecl_unified_summary_file , false , endian_convert); */
/*   else */
/*     ecl_sum->data         = ecl_fstate_alloc_empty(fmt_mode , ecl_summary_file , report_mode , endian_convert); */

/*   ecl_sum->base_name    = calloc(strlen(base_name) + 1 , sizeof *ecl_sum->base_name); */
/*   ecl_sum->Nwells       = Nwells; */
/*   ecl_sum->Nvars        = Nvars; */
/*   ecl_sum->param_offset = param_offset; */
/*   strcpy(ecl_sum->base_name , base_name); */
/*   { */
/*     const int size = param_offset + Nwells * Nvars; */
/*     bool FMT_FILE; */
/*     if (ecl_sum->fmt_mode == ECL_FORMATTED)  */
/*       FMT_FILE = true; */
/*     else */
/*       FMT_FILE = false; */
    
/*     ecl_block_type *header_block = ecl_block_alloc(0 , FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *kw       = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *units    = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *restart  = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *dimens   = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *wells    = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *nums     = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     ecl_kw_type *startdat = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */

/*     /\* */
/*       Might not need these ??? */
/*       ecl_kw_type *runtimeI = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*       ecl_kw_type *runtimeD = ecl_kw_alloc_empty(FMT_FILE , ecl_sum->endian_convert); */
/*     *\/ */
    
/*     ecl_kw_set_header_alloc(kw       , "KEYWORDS" , size , "CHAR"); */
/*     ecl_kw_set_header_alloc(units    , "UNITS"    , size , "CHAR"); */
/*     ecl_kw_set_header_alloc(restart  , "RESTART"  , 9    , "CHAR"); */
/*     ecl_kw_set_header_alloc(dimens   , "DIMENS"   , 6    , "INTE"); */
/*     ecl_kw_set_header_alloc(wells    , "WGNAMES"  , size , "CHAR"); */
/*     ecl_kw_set_header_alloc(nums     , "NUMS"     , size , "INTE"); */
/*     ecl_kw_set_header_alloc(startdat , "STARTDAT" , 3    , "INTE"); */
    
/*     ecl_block_add_kw(header_block , restart  , COPY); */
/*     ecl_block_add_kw(header_block , dimens   , COPY); */
/*     ecl_block_add_kw(header_block , kw       , COPY); */
/*     ecl_block_add_kw(header_block , wells    , COPY); */
/*     ecl_block_add_kw(header_block , nums     , COPY); */
/* ecl_block_add_kw(header_block , units    , COPY); */
/* ecl_block_add_kw(header_block , startdat , COPY); */
    
/*     ecl_fstate_add_block(ecl_sum->header , header_block); */
/*   } */
/*   return ecl_sum; */
/* } */



void ecl_sum_set_header_data(ecl_sum_type *ecl_sum , const char *kw , void *value_ptr) {
  ecl_block_type *   block = ecl_fstate_iget_block(ecl_sum->header , 0 );
  ecl_kw_type     * ecl_kw = ecl_block_get_kw(block , kw);
  ecl_kw_set_memcpy_data(ecl_kw , value_ptr);
}



/*
  Format

  WGNAMES = 
  Dummy1 Dummy2 Dummy3  Well1 Well2 Well3
  Well4 Well1 Well2 Well3 Well4 Well1 Well2 ....
  

  KEYWORDS = 
  
*/



void ecl_sum_set_well_header(ecl_sum_type *ecl_sum, const char **_well_list) {
  ecl_block_type * block   = ecl_fstate_iget_block(ecl_sum->header , 0);
  ecl_kw_type     * ecl_kw = ecl_block_get_kw(block , "WGNAMES");
  {
    const char null_char = '\0';
    char *well_list = malloc(ecl_kw_get_size(ecl_kw) * (1 + ecl_str_len));
    char *well;
    int iw , ivar;
    for (iw = 0; iw < ecl_sum->param_offset; iw++) {
      well = &well_list[iw * (1 + ecl_str_len)];
      sprintf(well , ECL_DUMMY_WELL);
    }

    for (ivar = 0; ivar < ecl_sum->Nvars; ivar++) {
      for (iw = 0; iw < ecl_sum->Nwells; iw++) {
	well = &well_list[(ecl_sum->param_offset + ivar*ecl_sum->Nwells + iw) * (1 + ecl_str_len)];
	strcpy(well , _well_list[iw]);
	well[ecl_str_len] = null_char;
      }
    }
    
    ecl_kw_set_memcpy_data(ecl_kw , well_list);
    free(well_list);
  }
}





void ecl_sum_init_save(ecl_sum_type * ecl_sum , const char * base_name , int fmt_mode , bool unified) {
  ecl_sum->base_name = calloc(strlen(base_name) + 1 , sizeof *ecl_sum->base_name);
  strcpy(ecl_sum->base_name , base_name);

  ecl_sum_set_fmt_mode(ecl_sum , fmt_mode);
  ecl_sum_set_unified(ecl_sum , unified);
}


void ecl_sum_save(const ecl_sum_type * ecl_sum) {
  char *summary_spec , ext[2] , *data_file;
  bool fmt_file;
  if (ecl_sum->base_name == NULL || !(ecl_sum->fmt_mode == ECL_FORMATTED || ecl_sum->fmt_mode == ECL_BINARY)) {
    fprintf(stderr,"%s: must inititialise ecl_sum object prior to saving - aborting \n",__func__);
    abort();
  }
  
  if (ecl_sum->fmt_mode == ECL_FORMATTED) {
    fmt_file = true;
  } else {
    fmt_file = false;
    sprintf(ext , "S");
  }
  summary_spec = ecl_util_alloc_filename(NULL , ecl_sum->base_name ,  ecl_summary_header_file , fmt_file , -1);
  ecl_fstate_set_files(ecl_sum->header , 1 , (const char **) &summary_spec);
  

  if (ecl_sum->unified) {
    data_file = ecl_util_alloc_filename(NULL , ecl_sum->base_name ,  ecl_unified_summary_file , fmt_file , -1);
    ecl_fstate_set_files(ecl_sum->data , 1 , (const char **) &data_file);
    free(data_file);
  } else {
    int files , report_nr1 , report_nr2;
    char **filelist;
    
    files = ecl_fstate_get_report_size(ecl_sum->data , &report_nr1 , &report_nr2);
    filelist = ecl_util_alloc_simple_filelist(NULL , ecl_sum->base_name , ecl_summary_file , fmt_file , report_nr1 , report_nr2);
    ecl_fstate_set_files(ecl_sum->data , files , (const char **) filelist);
    util_free_string_list(filelist , files);
  }
  
  ecl_fstate_save(ecl_sum->header);
  ecl_fstate_save(ecl_sum->data);
  free(summary_spec);
}



static void ecl_sum_assert_index(const ecl_kw_type *params_kw, int index) {
  if (index < 0 || index >= ecl_kw_get_size(params_kw)) {
    fprintf(stderr,"%s index:%d invalid - aborting \n",__func__ , index);
    abort();
  }
}


double ecl_sum_iget2(const ecl_sum_type *ecl_sum , int time_index , int sum_index) {
  if (ecl_sum->data == NULL) {
    fprintf(stderr,"%s: data not loaded - aborting \n",__func__);
    abort();
  }
  {
    ecl_block_type * block    = ecl_fstate_get_block(ecl_sum->data , time_index);
    ecl_kw_type    * data_kw  = ecl_block_get_kw(block , "PARAMS");
    ecl_sum_assert_index(data_kw , sum_index);
    
    /* PARAMS underlying data type is float. */
    return (double) ecl_kw_iget_float(data_kw , sum_index);    
  }
}


int ecl_sum_get_index(const ecl_sum_type * ecl_sum , const char * well , const char *var) {
  int index = -1;

  if (hash_has_key(ecl_sum->index_hash , well)) {
    hash_type * var_hash = hash_get(ecl_sum->index_hash , well);
    if (hash_has_key(var_hash , var))
      index = hash_get_int(var_hash , var); 
    else {
      fprintf(stderr,"%s summary object does not have well/variable combination: %s/%s  \n",__func__ , well , var);
      /* abort(); */
    }   
  } else {
    fprintf(stderr,"%s summary object does not contain well: %s \n",__func__ , well);
    /* abort(); */
  }
  
  return index;
}



bool ecl_sum_has_well_var(const ecl_sum_type * ecl_sum , const char * well , const char *var) {
  if (hash_has_key(ecl_sum->index_hash , well)) {
    hash_type * var_hash = hash_get(ecl_sum->index_hash , well);
    if (hash_has_key(var_hash , var))
      return true;
    else 
      return false;
  } else 
    return false;
}



bool ecl_sum_has_var(const ecl_sum_type * ecl_sum , const char *var) {
  if (hash_has_key(ecl_sum->kw_index_hash , var))
    return true;
  else
    return false;
}


const char * ecl_sum_get_unit_ref(const ecl_sum_type * ecl_sum , const char *var) {
  if (hash_has_key(ecl_sum->unit_hash , var))
    return hash_get(ecl_sum->unit_hash , var);
  else {
    fprintf(stderr,"%s: variable:%s not defined - aborting \n",__func__ , var);
    abort();
  }
}


double ecl_sum_iget1(const ecl_sum_type *ecl_sum , int time_index , const char *well_name , const char *var_name , int *_sum_index) {
  int sum_index;
  double value;
  
  sum_index = ecl_sum_get_index(ecl_sum , well_name , var_name);
  value     = ecl_sum_iget2(ecl_sum , time_index , sum_index);
  
  if (_sum_index != NULL)
    *_sum_index = sum_index;
  return value;
}


double ecl_sum_iget(const ecl_sum_type *ecl_sum , int time_index , const char *well_name , const char *var_name) {
  return ecl_sum_iget1(ecl_sum , time_index , well_name , var_name , NULL);
}



void ecl_sum_set_fmt_mode(ecl_sum_type *ecl_sum , int fmt_mode) {
  if (ecl_sum->fmt_mode != fmt_mode) {
    ecl_sum->fmt_mode = fmt_mode;
    if (ecl_sum->header != NULL) ecl_fstate_set_fmt_mode(ecl_sum->header , fmt_mode);
    if (ecl_sum->data   != NULL) ecl_fstate_set_fmt_mode(ecl_sum->data , fmt_mode);
  }
}



int ecl_sum_get_Nwells(const ecl_sum_type *ecl_sum) {
  return ecl_sum->Nwells;
}


void ecl_sum_copy_well_names(const ecl_sum_type *ecl_sum , char **well_list) {
  int iw;

  for (iw=0; iw < ecl_sum->Nwells; iw++) 
    strcpy(well_list[iw] , ecl_sum->well_list[iw]);

}


char ** ecl_sum_alloc_well_names_copy(const ecl_sum_type *ecl_sum) {
  char **well_list;
  int iw;
  well_list = calloc(ecl_sum->Nwells , sizeof *well_list);
  for (iw = 0; iw < ecl_sum->Nwells; iw++) {
    well_list[iw] = malloc(strlen(ecl_sum->well_list[iw]) + 1);
    well_list[iw][0] = '\0';
  }
  ecl_sum_copy_well_names(ecl_sum , well_list);
  return well_list;
}


const char ** ecl_sum_get_well_names_ref(const ecl_sum_type * ecl_sum) {
  return (const char **) ecl_sum->well_list;
}


int ecl_sum_get_size(const ecl_sum_type *ecl_sum) {
  return ecl_fstate_get_size(ecl_sum->data);
}


bool ecl_sum_get_report_mode(const ecl_sum_type * ecl_sum) {
  return ecl_fstate_get_report_mode(ecl_sum->data);
}


time_t ecl_sum_get_start_time(const ecl_sum_type * ecl_sum) {
  return ecl_sum->sim_start_time;
}


time_t ecl_sum_get_sim_time(const ecl_sum_type * ecl_sum , int index) {
  ecl_block_type * block = ecl_fstate_get_block(ecl_sum->data , index);
  return ecl_block_get_sim_time(block);
}


int ecl_sum_get_report_size(const ecl_sum_type * ecl_sum , int * first_report_nr , int * last_report_nr) {
  return ecl_fstate_get_report_size(ecl_sum->data , first_report_nr , last_report_nr);
}


double ecl_sum_eval_well_misfit(const ecl_sum_type * ecl_sum , const char * well , int nvar , const char ** var_list , const double * inv_covar) {
  double  R2;
  double *residual , *tmp;
  char **hvar_list;
  int istep,ivar;

  hvar_list = malloc(nvar * sizeof * hvar_list);
  for (ivar = 0; ivar < nvar; ivar++) {
    hvar_list[ivar] = malloc(strlen(var_list[ivar]) + 2);
    sprintf(hvar_list[ivar] , "%sH" , var_list[ivar]);
  }
  residual = malloc(nvar * sizeof * residual);
  tmp      = malloc(nvar * sizeof * tmp);

  R2 = 0;
  for (istep = 0; istep < ecl_sum_get_size(ecl_sum);  istep++) {
    double history_value , value;
    for (ivar = 0; ivar < nvar; ivar++) {
      if (ecl_sum_has_well_var(ecl_sum , well , hvar_list[ivar])) {
	history_value = ecl_sum_iget1(ecl_sum , istep , well , hvar_list[ivar] , NULL);
	value         = ecl_sum_iget1(ecl_sum , istep , well , var_list[ivar]  , NULL);
	
	residual[ivar] = (history_value - value);
      }
    }
    
    {
      int i,j;
      for (i = 0; i < nvar; i++) {
	tmp[i] = 0;
	for (j = 0; j < nvar; j++) 
	  tmp[i] += residual[j] * inv_covar[j*nvar + i];
	R2 += tmp[i] * residual[i];
      }
    }
  }
  
  free(tmp);
  free(residual);
  util_free_string_list(hvar_list , nvar);
  return R2;
}


void ecl_sum_well_max_min(const ecl_sum_type * ecl_sum, const char * well , int nvar , const char ** var_list , double *max , double *min, bool init) {
  int istep,ivar;

  if (init) {
    for (ivar = 0; ivar < nvar; ivar++) {
      max[ivar] = -1e100;
      min[ivar] =  1e100;
    }
  }

  for (istep = 0; istep < ecl_sum_get_size(ecl_sum);  istep++) {
    for (ivar = 0; ivar < nvar; ivar++) {
      if (ecl_sum_has_well_var(ecl_sum , well , var_list[ivar])) {
	double value = ecl_sum_iget(ecl_sum , istep , well , var_list[ivar]);
	min[ivar]  = util_double_min(min[ivar] , value);
	max[ivar]  = util_double_max(max[ivar] , value);
      }
    }
  }
}



void ecl_sum_max_min(const ecl_sum_type * ecl_sum, int nwell , const char ** well_list , int nvar , const char ** var_list , double *max , double *min, bool init_maxmin) {
  int iwell;
  ecl_sum_well_max_min(ecl_sum , well_list[0] , nvar , var_list , max , min , init_maxmin);
  for (iwell = 1; iwell < nwell; iwell++) 
    ecl_sum_well_max_min(ecl_sum , well_list[iwell] , nvar , var_list , max , min , false);
  
}


double ecl_sum_eval_misfit(const ecl_sum_type * ecl_sum , int nwell , const char ** well_list , int nvar , const char ** var_list , const double * inv_covar, double * misfit) {
  int    iwell;
  double total_misfit = 0;
  for (iwell = 0; iwell < nwell; iwell++) {
    misfit[iwell] = ecl_sum_eval_well_misfit(ecl_sum , well_list[iwell] , nvar , var_list , inv_covar);
    total_misfit += misfit[iwell];
  }
  return total_misfit;
}





void ecl_sum_free_data(ecl_sum_type * ecl_sum) {
  ecl_fstate_free(ecl_sum->data);
  ecl_sum->data = NULL;
}


void ecl_sum_free(ecl_sum_type *ecl_sum) {
  ecl_sum_free_data(ecl_sum);
  ecl_fstate_free(ecl_sum->header);
  hash_free(ecl_sum->index_hash);
  hash_free(ecl_sum->kw_index_hash);
  hash_free(ecl_sum->unit_hash);
  util_free_string_list(ecl_sum->well_list  , ecl_sum->Nwells);

  if (ecl_sum->base_name != NULL)
    free(ecl_sum->base_name);
  free(ecl_sum);
}



/*****************************************************************/

ecl_sum_type * ecl_sum_fread_alloc_interactive(bool endian_convert) {
  char ** file_list   = NULL;
  char *  header_file = NULL;
  int     files;
  char * base;
  char   path[256];
  bool   report_mode = true;
  ecl_sum_type * ecl_sum;
  
  util_read_path("Directory to load ECLIPSE summary from" , 50 , true , path);
  base = ecl_util_alloc_base_guess(path);
  if (base == NULL) {
    base = util_malloc(9 , __func__);
    util_read_string("Basename for ECLIPSE simulation" , 50 , base);
  } else 
    printf("%-50s: %s\n" , "Using ECLIPSE base",base);
  
  {
    int formatted_files;
    int unformatted_files;
    int possibilities = 0;

    char * unified_formatted   = ecl_util_alloc_filename(path , base , ecl_unified_summary_file , true  , 0 );
    char * unified_unformatted = ecl_util_alloc_filename(path , base , ecl_unified_summary_file , false , 0 );

    char ** formatted_list     = ecl_util_alloc_scandir_filelist(path , base , ecl_summary_file , true  , &formatted_files  );
    char ** unformatted_list   = ecl_util_alloc_scandir_filelist(path , base , ecl_summary_file , false , &unformatted_files);
    
    char * formatted_header    = ecl_util_alloc_filename(path , base , ecl_summary_header_file , true  , 0 );
    char * unformatted_header  = ecl_util_alloc_filename(path , base , ecl_summary_header_file , false , 0 );
    
    if (util_file_exists(unified_formatted))   possibilities++;
    if (util_file_exists(unified_unformatted)) possibilities++;
    if (formatted_files > 0)                   possibilities++;
    if (unformatted_files > 0)                 possibilities++;

    if (possibilities == 0) {
      fprintf(stderr,"** WARNING: Could not find summary data in directory: %s ** \n",path);
      files     = 0;
      file_list = NULL;
      header_file = NULL;
    } else if (possibilities == 1) {
      if (possibilities == 1) {
	if (util_file_exists(unified_formatted)) {
	  files = 1;
	  file_list   = util_alloc_stringlist_copy((const char **) &unified_formatted , 1);
	  header_file = util_alloc_string_copy(formatted_header);
	} else if (util_file_exists(unified_unformatted)) {
	  files = 1;
	  file_list   = util_alloc_stringlist_copy((const char **) &unified_unformatted , 1);
	  header_file = util_alloc_string_copy(unformatted_header);
	} else if (formatted_files > 0) {
	  files = formatted_files;
	  file_list   = util_alloc_stringlist_copy((const char **) formatted_list , formatted_files);
	  header_file = util_alloc_string_copy(formatted_header);
	} else if (unformatted_files > 0) {
	  files = unformatted_files;
	  file_list   = util_alloc_stringlist_copy((const char **) unformatted_list , unformatted_files);
	  header_file = util_alloc_string_copy(unformatted_header);
	}
      }
    } else {
      bool fmt , unified;
      /* Should be read in interactively */  

      unified = false;
      fmt     = false;
      if (unified) {
	files = 1;
	file_list = malloc(sizeof * file_list);
	file_list[0] = ecl_util_alloc_exfilename(path , base , ecl_unified_summary_file , fmt  , 0 );
      } else 
	file_list = ecl_util_alloc_scandir_filelist(path , base , ecl_summary_file , fmt , &files);

      if (fmt)
	header_file = util_alloc_string_copy(formatted_header);
      else
	header_file = util_alloc_string_copy(unformatted_header);
      
    }
    free(formatted_header);
    free(unformatted_header);
    free(unified_formatted);
    free(unified_unformatted);
    util_free_string_list(unformatted_list , unformatted_files);
    util_free_string_list(formatted_list   , formatted_files);
  }
  
  if (files > 0)
    ecl_sum = ecl_sum_fread_alloc(header_file , files , (const char ** ) file_list , report_mode , endian_convert);
  else
    ecl_sum = NULL;

  util_free_string_list(file_list , files); 
  if (header_file != NULL) free(header_file);
  free(base);

  return ecl_sum;
}
