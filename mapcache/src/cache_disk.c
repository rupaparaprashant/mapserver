/*
 * disk_cache.c
 *
 *  Created on: Oct 10, 2010
 *      Author: tom
 */

#include "yatc.h"
#include <apr_file_info.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <http_log.h>

int _yatc_cache_disk_tile_key(request_rec *r, yatc_tile *tile, char **path) {
   *path = apr_psprintf(r->pool,"%s/%s/%02d/%03d/%03d/%03d/%03d/%03d/%03d.%s",
         ((yatc_cache_disk*)tile->tileset->cache)->base_directory,
         tile->tileset->name,
         tile->z,
         tile->x / 1000000,
         (tile->x / 1000) % 1000,
         tile->x % 1000,
         tile->y / 1000000,
         (tile->y / 1000) % 1000,
         tile->y % 1000,
         (tile->tileset->source->image_format == YATC_IMAGE_FORMAT_PNG)?"png":"jpg");
   return YATC_SUCCESS;
}

int _yatc_cache_disk_tile_key_split(request_rec *r, yatc_tile *tile, char **path, char **basename) {
   *path = apr_psprintf(r->pool,"%s/%s/%02d/%03d/%03d/%03d/%03d/%03d",
         ((yatc_cache_disk*)tile->tileset->cache)->base_directory,
         tile->tileset->name,
         tile->z,
         tile->x / 1000000,
         (tile->x / 1000) % 1000,
         tile->x % 1000,
         tile->y / 1000000,
         (tile->y / 1000) % 1000);
   *basename = apr_psprintf(r->pool,"%03d.%s",tile->y % 1000,
         (tile->tileset->source->image_format == YATC_IMAGE_FORMAT_PNG)?"png":"jpg");
   return YATC_SUCCESS;
}

int _yatc_cache_disk_create_and_lock(yatc_tile *tile, request_rec *r) {
   char *filename;
   char *basename;
   char *dirname;
   apr_file_t *f;
   apr_status_t rv;
   _yatc_cache_disk_tile_key_split(r, tile, &dirname, &basename);
   rv = apr_dir_make_recursive(dirname,APR_OS_DEFAULT,r->pool);
   if(rv != APR_SUCCESS) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to create directory %s",dirname);
      return YATC_FAILURE;
   }
   filename = apr_psprintf(r->pool,"%s/%s",dirname,basename);
   /*create file, and fail if it already exists*/
   if(apr_file_open(&f, filename,
         APR_FOPEN_CREATE|APR_FOPEN_EXCL|APR_FOPEN_WRITE|APR_FOPEN_SHARELOCK|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,
         APR_OS_DEFAULT, r->pool) != APR_SUCCESS) {
      /* 
       * opening failed, is this because we don't have write permissions, or because 
       * the file already exists?
       */
      if(apr_file_open(&f, filename, APR_FOPEN_CREATE|APR_FOPEN_WRITE, APR_OS_DEFAULT, r->pool) != APR_SUCCESS) {
         ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to create file %s",filename);
         return YATC_FAILURE; /* we could not create the file */
      } else {
         /* this shouldn't happen if the caller has properly mutex protected the call ? */
         ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "asked to create file %s, but it already exists",filename);
         apr_file_close(f);
         return YATC_FILE_EXISTS; /* we have write access, but the file already exists */
      }
   }
   rv = apr_file_lock(f, APR_FLOCK_EXCLUSIVE|APR_FLOCK_NONBLOCK);
   if(rv != APR_SUCCESS) {
      if(rv == EAGAIN) {
         ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "####### TILE LOCK ######## file %s is already locked",filename);
      } else {
         ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to lock file %s",filename);
      }
      return YATC_FAILURE;
   }
   tile->lock = f;
   return YATC_SUCCESS;
}

int _yatc_cache_disk_unlock(yatc_tile *tile, request_rec *r) {
   if(!tile->lock) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "attempting to unlock an already unlocked tile");
      return YATC_FAILURE;
   }
   apr_finfo_t finfo;
   apr_file_t *f = (apr_file_t*)tile->lock;
   apr_file_info_get(&finfo, APR_FINFO_SIZE, f);
   if(!finfo.size) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "unlocking an empty tile, we will remove it");
      const char *fname;
      apr_file_name_get(&fname,f);
      apr_file_remove(fname,r->pool);
   }
   int rv = apr_file_unlock(f);
   if(rv != APR_SUCCESS) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to unlock file");
      return YATC_FAILURE;
   }
   rv = apr_file_close(f);
   if(rv != APR_SUCCESS) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to close file");
      return YATC_FAILURE;
   }
   tile->lock = NULL;
   return rv;
}

int _yatc_cache_disk_get(yatc_tile *tile, request_rec *r) {
   char *filename;
   apr_file_t *f;
   apr_finfo_t finfo;
   apr_status_t rv;
   apr_size_t size;
   _yatc_cache_disk_tile_key(r, tile, &filename);
   if(apr_file_open(&f, filename, APR_FOPEN_READ|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,
         APR_OS_DEFAULT, r->pool) != APR_SUCCESS) {
      /* the file doesn't exist on the disk */
      return YATC_CACHE_MISS;
   }
   apr_file_info_get(&finfo, APR_FINFO_SIZE, f);
   if(!finfo.size) {
      /* 
       * the file exists on the disk, but it has 0 length. This is normally because another
       * thread / process has indicated it is doing the rendering.
       * we put a shared lock on the resource, to wait until the other thread has finished
       */
      rv = apr_file_lock(f, APR_FLOCK_SHARED);
      if(rv != APR_SUCCESS) {
         const char *filename;
         apr_file_name_get(&filename,f);
         ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "failed to set read lock on %s",filename);
         return YATC_FAILURE;
      }
      apr_file_unlock(f);
      /* should we reopen the file now ? */
      apr_file_info_get(&finfo, APR_FINFO_SIZE, f);
      if(!finfo.size) {
         const char *filename;
         apr_file_name_get(&filename,f);
         ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "tile %s has no data",filename);
         return YATC_FAILURE;
      }
   }
   size = finfo.size;
   /*
    * at this stage, we have a handle to an open file that contains data.
    * idealy, we should aquire a read lock, in case the data contained inside the file
    * is incomplete.
    * currently such a lock is not set, as we don't want to loose performance on tile accesses.
    * any error that might happen at this stage should only occur if the tile isn't already cached,
    * i.e. normally only once.
    */
   tile->data = yatc_buffer_create(size,r->pool);
   //manually add the data to our buffer
   apr_file_read(f,(void*)tile->data->buf,&size);
   tile->data->size = size;
   apr_file_close(f);
   if(size != finfo.size) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to copy image data, got %d of %d bytes",(int)size, (int)finfo.size);
      return YATC_FAILURE;
   }

   return YATC_SUCCESS;
}

int _yatc_cache_disk_set(yatc_tile *tile, request_rec *r) {
   apr_size_t bytes;
   apr_file_t *f = (apr_file_t*)tile->lock;
#ifdef DEBUG
   /* all this should be checked at a higher level */
   if(!tile->data || !tile->data->size) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "attempting to write empty tile to disk");
      return YATC_FAILURE;
   }
   if(!f) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "attempting to write to an unlocked tile");
      return YATC_FAILURE;
   }
#endif
   bytes = (apr_size_t)tile->data->size;
   apr_file_write(f,(void*)tile->data->buf,&bytes);

   if(bytes != tile->data->size) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed to write image data to disk, wrote %d of %d bytes",(int)bytes, (int)tile->data->size);
      return YATC_FAILURE;
   }
   return YATC_SUCCESS;
}

char* _yatc_cache_disk_configuration_parse(xmlNode *xml, yatc_cache *cache, apr_pool_t *pool) {
   xmlNode *cur_node;
   yatc_cache_disk *dcache = (yatc_cache_disk*)cache;
   for(cur_node = xml->children; cur_node; cur_node = cur_node->next) {
      if(cur_node->type != XML_ELEMENT_NODE) continue;
      if(!xmlStrcmp(cur_node->name, BAD_CAST "base")) {
         xmlChar* value = xmlNodeGetContent(cur_node);
         dcache->base_directory = (char*)value;
      }
   }
   return NULL;
}

char* _yatc_cache_disk_configuration_check(yatc_cache *cache, apr_pool_t *pool) {
   apr_status_t status;
   apr_dir_t *dir;
   yatc_cache_disk *dcache = (yatc_cache_disk*)cache;
   /* check all required parameters are configured */
   if(!dcache->base_directory || !strlen(dcache->base_directory)) {
      return apr_psprintf(pool,"disk cache %s has no base directory",dcache->cache.name);
   }

   status = apr_dir_open(&dir, dcache->base_directory, pool);
   if(status != APR_SUCCESS) {
      return apr_psprintf(pool, "failed to access directory %s for cache %s",
            dcache->base_directory, dcache->cache.name);
   }
   /* TODO: more checks on directory readability/writability */
   apr_dir_close(dir);
   return NULL;
}

yatc_cache_disk* yatc_cache_disk_create(apr_pool_t *pool) {
   yatc_cache_disk *cache = apr_pcalloc(pool,sizeof(yatc_cache_disk));
   cache->cache.type = YATC_CACHE_DISK;
   cache->cache.tile_get = _yatc_cache_disk_get;
   cache->cache.tile_set = _yatc_cache_disk_set;
   cache->cache.configuration_check = _yatc_cache_disk_configuration_check;
   cache->cache.configuration_parse = _yatc_cache_disk_configuration_parse;
   cache->cache.tile_lock = _yatc_cache_disk_create_and_lock;
   cache->cache.tile_unlock = _yatc_cache_disk_unlock;
   return cache;
}

