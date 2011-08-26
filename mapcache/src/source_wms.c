#include "yatc.h"
#include <libxml/tree.h>
#include <apr_tables.h>
#include <apr_strings.h>


int _yatc_source_wms_render_tile(yatc_tile *tile, request_rec *r) {
   yatc_wms_source *wms = (yatc_wms_source*)tile->tileset->source;
   apr_table_t *params = apr_table_clone(r->pool,wms->wms_default_params);
   double bbox[4];
   yatc_tileset_tile_bbox(tile,bbox);
   apr_table_setn(params,"BBOX",apr_psprintf(r->pool,"%f,%f,%f,%f",bbox[0],bbox[1],bbox[2],bbox[3]));
   apr_table_setn(params,"WIDTH",apr_psprintf(r->pool,"%d",tile->sx));
   apr_table_setn(params,"HEIGHT",apr_psprintf(r->pool,"%d",tile->sy));
   apr_table_setn(params,"FORMAT","image/png");
   apr_table_setn(params,"SRS",tile->tileset->srs);
   
   apr_table_overlap(params,wms->wms_params,0);
        
   tile->data = yatc_buffer_create(1000,r->pool);
   yatc_http_request_url_with_params(r,wms->url,params,tile->data);
      
   return YATC_SUCCESS;
}

int _yatc_source_wms_render_metatile(yatc_metatile *tile, request_rec *r) {
   yatc_wms_source *wms = (yatc_wms_source*)tile->tile.tileset->source;
   apr_table_t *params = apr_table_clone(r->pool,wms->wms_default_params);
   apr_table_setn(params,"BBOX",apr_psprintf(r->pool,"%f,%f,%f,%f",
         tile->bbox[0],tile->bbox[1],tile->bbox[2],tile->bbox[3]));
   apr_table_setn(params,"WIDTH",apr_psprintf(r->pool,"%d",tile->tile.sx));
   apr_table_setn(params,"HEIGHT",apr_psprintf(r->pool,"%d",tile->tile.sy));
   apr_table_setn(params,"FORMAT","image/png");
   apr_table_setn(params,"SRS",tile->tile.tileset->srs);
   
   apr_table_overlap(params,wms->wms_params,0);
        
   tile->tile.data = yatc_buffer_create(30000,r->pool);
   yatc_http_request_url_with_params(r,wms->url,params,tile->tile.data);
      
   return YATC_SUCCESS;
}

char* _yatc_source_wms_configuration_parse(xmlNode *xml, yatc_source *source, apr_pool_t *pool) {
   xmlNode *cur_node;
   yatc_wms_source *src = (yatc_wms_source*)source;
   for(cur_node = xml->children; cur_node; cur_node = cur_node->next) {
      if(cur_node->type != XML_ELEMENT_NODE) continue;
      if(!xmlStrcmp(cur_node->name, BAD_CAST "url")) {
         char* value = (char*)xmlNodeGetContent(cur_node);
         src->url = value;
      } else if(!xmlStrcmp(cur_node->name, BAD_CAST "wmsparams")) {
         xmlNode *param_node;
         for(param_node = cur_node->children; param_node; param_node = param_node->next) {
            char *key,*value;
            if(param_node->type != XML_ELEMENT_NODE) continue;
            value = (char*)xmlNodeGetContent(param_node);
            key = apr_pstrdup(pool, (char*)param_node->name);
            apr_table_setn(src->wms_params, key, value);
         }
      }
   }
   return NULL;
}

char* _yatc_source_wms_configuration_check(yatc_source *source, apr_pool_t *pool) {
   yatc_wms_source *src = (yatc_wms_source*)source;
   /* check all required parameters are configured */
   if(!strlen(src->url)) {
      return apr_psprintf(pool,"wms source %s has no url",source->name);
   }
   if(!apr_table_get(src->wms_params,"LAYERS"))
      return apr_psprintf(pool,"wms source %s has no LAYERS", source->name);


   return NULL;
}

yatc_wms_source* yatc_source_wms_create(apr_pool_t *pool) {
   yatc_wms_source *source = apr_pcalloc(pool, sizeof(yatc_wms_source));
   yatc_source_init(&(source->source),pool);
   source->source.type = YATC_SOURCE_WMS;
   source->source.supports_metatiling = 1;
   source->source.render_tile = _yatc_source_wms_render_tile;
   source->source.render_metatile = _yatc_source_wms_render_metatile;
   source->source.configuration_check = _yatc_source_wms_configuration_check;
   source->source.configuration_parse = _yatc_source_wms_configuration_parse;
   source->wms_default_params = apr_table_make(pool,4);;
   source->wms_params = apr_table_make(pool,4);
   apr_table_add(source->wms_default_params,"VERSION","1.1.1");
   apr_table_add(source->wms_default_params,"REQUEST","GetMap");
   apr_table_add(source->wms_default_params,"SERVICE","WMS");
   apr_table_add(source->wms_default_params,"STYLES","");
   return source;
}



