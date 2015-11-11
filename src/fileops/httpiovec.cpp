/*
 * This File is part of Davix, The IO library for HTTP based protocols
 * Copyright (C) CERN 2013  
 * Author: Adrien Devresse <adrien.devresse@cern.ch>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
*/

#include <davix_internal.hpp>
#include "httpiovec.hpp"
#include <utils/davix_logger_internal.hpp>
#include <string_utils/stringutils.hpp>

#include <map>

using namespace StrUtil;

// remove trailing crlf
template<class InputIterator>
  dav_size_t trim_crlf (InputIterator first, InputIterator last, dav_size_t s)
{
  dav_size_t ret = s;
  while (--last >= first) {
      if(*last == '\n' || *last == '\r'){
          *last = '\0';
          ret--;
      }else{
          break;
      }
  }
  return ret;
}



namespace Davix{

const std::string HttpIoVec_scope(){
    return "Davix::HttpIOVecOps";
}


void HttpIoVecSetupErrorMultiPart(DavixError** err){
    DavixError::setupError(err, HttpIoVec_scope(), StatusCode::InvalidServerResponse, "Invalid Multi-Part HTTP response");
}

void HttpIoVecSetupErrorMultiPartTooLong(DavixError** err){
    DavixError::setupError(err, HttpIoVec_scope(), StatusCode::InvalidServerResponse, "Invalid Multi-Part HTTP, Multi-part header too long");
}

void HttpIoVecSetupErrorMultiPartBoundary(const std::string & boundary, DavixError** err){
    DavixError::setupError(err, HttpIoVec_scope(), StatusCode::InvalidServerResponse, std::string("Invalid boundary for multipart http reponse :").append(boundary));
}

void HttpIoVecSetupErrorMultiPartSize( DavixError** err, dav_off_t req_offset, dav_size_t req_size, dav_off_t ans_offset, dav_size_t ans_size){
    std::ostringstream ss;
    ss << "Invalid server answer for multi part, request offset:"<< req_offset <<" size:"<< req_size << ", answer offset:"<< ans_offset<< " size:"<< ans_size;
    DavixError::setupError(err, HttpIoVec_scope(), StatusCode::InvalidServerResponse, ss.str());
}

inline char* header_delimiter(char* buffer, dav_size_t len){
    char* p = std::find(buffer, buffer + len, ':');
    return (p < buffer + len)?p:NULL;
}

// Vector operation option provider
int davIOVecProvider(const DavIOVecInput *input_vec, dav_ssize_t & counter, dav_ssize_t number, dav_off_t & begin, dav_off_t & end){
    if(counter < number){
        begin = input_vec[counter].diov_offset;
        end = std::max<dav_off_t>(begin + input_vec[counter].diov_size -1, begin);
        return ++counter;
    }
    return -1;
}

// do a multi-range on selected ranges
MultirangeResult HttpIOVecOps::performMultirange(IOChainContext & iocontext,
                                     const DavIOVecInput * input_vec,
                                     DavIOVecOuput * output_vec,
                                     const dav_size_t count_vec) {
    DavixError * tmp_err=NULL;
    dav_ssize_t tmp_ret=-1, ret = 0;
    ptrdiff_t p_diff=0;
    dav_ssize_t counter = 0;
    MultirangeResult::OperationResult opresult = MultirangeResult::SUCCESS;

    // calculate total bytes to be read (approximate, since ranges could overlap)
    dav_ssize_t bytes_to_read = 0;
    for(dav_size_t i = 0; i < count_vec; i++) {
        bytes_to_read += (input_vec+i)->diov_size;
    }

    // generator of offset
    boost::function<int (dav_off_t &, dav_off_t &)> offsetProvider( boost::bind(davIOVecProvider, input_vec, counter, (dav_ssize_t) count_vec,
                         _1, _2));

    // header line need to be inferior to 8K on Apache2 / ngnix
    // in Addition, some S3 implementation limit the total header size to 4k....
    // 3900 bytes maximum for the range seems to be a ood compromise
    std::vector< std::pair<dav_size_t, std::string> > vecRanges = generateRangeHeaders(3900, offsetProvider);

    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, " -> getPartialVec operation for {} vectors", count_vec);

    for(std::vector< std::pair<dav_size_t, std::string> >::iterator it = vecRanges.begin(); it < vecRanges.end(); ++it){
        DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, " -> getPartialVec request for {} chunks", it->first);

        if(it->first == 1){ // one chunk only : no need of multi part
            ret += singleRangeRequest(iocontext, input_vec+p_diff, output_vec+p_diff);
            p_diff += 1;
        }else{
            GetRequest req (iocontext._context, iocontext._uri, &tmp_err);
            if(tmp_err == NULL){
                RequestParams request_params(iocontext._reqparams);
                req.setParameters(request_params);
                req.addHeaderField(req_header_byte_range, it->second);

                if( req.beginRequest(&tmp_err) == 0){
                    const int retcode = req.getRequestCode();

                    // looks like the server supports multi-range requests.. yay
                    if(retcode == 206) {
                        ret = parseMultipartRequest(req, input_vec+p_diff,
                                                    output_vec+p_diff, count_vec, &tmp_err);

                        // could not parse multipart response - server's broken?
                        // known to happen with ceph - return code is 206, but only
                        // returns the first range
                        if(ret == -1) {
                            opresult = MultirangeResult::NOMULTIRANGE;
                            req.endRequest(&tmp_err);
                            break;
                        }
                    }
                    // no multi-range.. bad server, bad
                    else if(retcode == 200) {
                        DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Multi-range request resulted in getting the whole file.");
                        // we have two options: read the entire file or abort current
                        // request and start a multi-range simulation

                        // if this is a huge file, reading the entire contents is
                        // definitely not an option
                        if(req.getAnswerSize() > 1000000 && req.getAnswerSize() > 2*bytes_to_read) {
                            DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "File is too large; will not waste bandwidth, bailing out");
                            opresult = MultirangeResult::NOMULTIRANGE;
                            req.endRequest(&tmp_err);
                        }
                        else {
                            DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Simulating multi-part response from the contents of the entire file");
                            opresult = MultirangeResult::SUCCESS_BUT_NO_MULTIRANGE;
                            ret = simulateMultiPartRequest(req, input_vec, output_vec, count_vec, &tmp_err);
                        }
                        break;
                    }
                    else {
                        httpcodeToDavixError(req.getRequestCode(),davix_scope_http_request(),", ", &tmp_err);
                        ret = -1;
                        break;
                    }

                    p_diff += it->first;
                    ret += tmp_ret;
                }
            } else {
                ret = -1;
                break;
            }
        }
    }

    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, " <- getPartialVec operation for {} vectors", count_vec);
    checkDavixError(&tmp_err);
    return MultirangeResult(opresult, ret);
}

/* fire off a single, one-range request */
dav_ssize_t HttpIOVecOps::singleRangeRequest(IOChainContext & iocontext,
                                     const DavIOVecInput * input,
                                     DavIOVecOuput * output) {
    dav_ssize_t size = _start->pread(iocontext,
                                     input->diov_buffer,
                                     input->diov_size,
                                     input->diov_offset);

    output->diov_size = size;
    output->diov_buffer = input->diov_buffer;
    return size;
}

dav_ssize_t HttpIOVecOps::simulateMultirange(IOChainContext & iocontext,
                                     const DavIOVecInput * input_vec,
                                     DavIOVecOuput * output_vec,
                                     const dav_size_t count_vec) {
    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Simulating a multi-range request with {} vectors", count_vec);
    dav_ssize_t size = 0;
    for(dav_size_t i = 0; i < count_vec; i++) {
        size += singleRangeRequest(iocontext, input_vec+i, output_vec+i);
    }
    return size;
}

dav_ssize_t HttpIOVecOps::preadVec(IOChainContext & iocontext, const DavIOVecInput * input_vec,
                          DavIOVecOuput * output_vec,
                          const dav_size_t count_vec){

    if(count_vec ==0)
        return 0;

    // a lot of servers do not support multirange... should we even try?
    if(count_vec == 1 || iocontext._uri.getFragmentParam("multirange") == "false") {
        return simulateMultirange(iocontext, input_vec, output_vec, count_vec);
    }

    MultirangeResult res = performMultirange(iocontext, input_vec, output_vec, count_vec);
    if(res.res == MultirangeResult::SUCCESS || res.res == MultirangeResult::SUCCESS_BUT_NO_MULTIRANGE) {
        return res.size_bytes;
    }
    else {
        DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Multi-range request has failed, attempting to recover by using multiple single-range requests");
        return simulateMultirange(iocontext, input_vec, output_vec, count_vec);
    }
}

dav_ssize_t HttpIOVecOps::readPartialBufferVecRequest(HttpRequest & _req,
                          const DavIOVecInput * input_vec,
                          DavIOVecOuput * output_vec,
                          const dav_size_t count_vec, DavixError** err){
    dav_ssize_t ret=-1;
    DavixError* tmp_err=NULL;
    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, " -> Davix Vector operation");
    if( _req.beginRequest(&tmp_err)  == 0){
        const int retcode = _req.getRequestCode();
        switch(retcode){
             case 206: // multipart req
                 ret = parseMultipartRequest(_req, input_vec,
                                          output_vec, count_vec, &tmp_err);
                 break;
             case 200: // classical req, simulate vector ops
                 ret = simulateMultiPartRequest(_req, input_vec, output_vec, count_vec, &tmp_err);
                 break;
             default:
                 httpcodeToDavixError(_req.getRequestCode(),davix_scope_http_request(),", ", &tmp_err);
        }
    }

    DavixError::propagateError(err, tmp_err);
    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, " <- Davix Vector operation");
    return ret;
}


int http_extract_boundary_from_content_type(const std::string & buffer, std::string & boundary, DavixError** err){
    dav_size_t pos_bound;
    static const std::string delimiter = "\";";
    if( (pos_bound= buffer.find(ans_header_boundary_field)) != std::string::npos){
        std::vector<std::string> tokens = tokenSplit(buffer.substr(pos_bound + ans_header_boundary_field.size()), delimiter);
        if( tokens.size() >= 1
            && tokens[0].size() > 0
            && tokens[0].size() <= 70){
            DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, "Multi part boundary: {}", boundary);
            std::swap(boundary,tokens[0]);
            return 0;
        }
    }
    return -1;
}


int get_multi_part_info(const HttpRequest& req, std::string & boundary, DavixError** err){

    std::string buffer;

    if( req.getAnswerHeader(ans_header_content_type, buffer) == true // has content type
           && http_extract_boundary_from_content_type(buffer, boundary, err) == 0){
          return 0;
    }
    return -1;
}






// analyze header and try to find size of the part
// return 0 -> not a content length header, return -1 : not a header or error, return 1 : success
int find_header_params(char* buffer, dav_size_t buffer_len, dav_size_t* part_size, dav_off_t* part_offset){
    static const std::string delimiter(" bytes-/\t");
    char * p = header_delimiter(buffer, buffer_len);
    if(p == NULL)
        return -1;
    std::string header_type(buffer, p - buffer);
    if( compare_ncase(ans_header_byte_range, 0, p - buffer, buffer) !=0) // check header type
        return 0;

    std::vector<std::string> tokens = tokenSplit(std::string(p+1),delimiter);     // parse header
    if(tokens.size() < 2)
        return -1;

    long chunk_size[2];
    for(int i =0; i <2;++i){
        chunk_size[i]= strtol(tokens[i].c_str(), &p, 10);
        if(chunk_size[i] == LONG_MAX || chunk_size[i] < 0 || *p != '\0'){
            errno =0;
            return -1;
        }
    }
    if(chunk_size[1] < chunk_size[0])
        return -1;

    *part_offset= chunk_size[0];
    *part_size =  chunk_size[1]-chunk_size[0]+1;
    return 1;
}

inline dav_ssize_t parse_multi_part_header_line(HttpRequest& req, char* buffer, DavixError** err){
    dav_ssize_t ret =0;
    if( (ret = req.readLine(buffer, DAVIX_READ_BLOCK_SIZE, err)) <0 ){
        return -1;
    }
    ret=  trim_crlf(buffer, buffer + ret, ret);
    return ret;
}

int  parse_multi_part_header(HttpRequest& req, const std::string & boundary, ChunkInfo & info,
                            int & n_try, DavixError** err){
    dav_ssize_t ret =0;
    char buffer[DAVIX_READ_BLOCK_SIZE+1] = {0};

    if(n_try > 100){
        HttpIoVecSetupErrorMultiPartTooLong(err);
        return -1;
    }

    if( (ret = parse_multi_part_header_line(req, buffer, err)) <0 ){
        return -1;
    }

    if(!info.bounded){
        if(ret == 0) // start with crlf
            return parse_multi_part_header(req, boundary, info, ++n_try, err);

        if( is_a_start_boundary_part(buffer, DAVIX_READ_BLOCK_SIZE, boundary, err) == false)
            return -1;
        info.bounded = true;
        return parse_multi_part_header(req, boundary, info, ++n_try, err);
    }

    if( info.offset == 0 && info.size == 0){
        if( find_header_params(buffer, ret, &(info.size), &(info.offset)) < 0)
            return -1;
        return parse_multi_part_header(req, boundary, info, ++n_try, err);
    }
    if(ret == 0) // end crlf
        return 0;
    HttpIoVecSetupErrorMultiPart(err);
    return -1;
}

dav_ssize_t copyChunk(HttpRequest & req, const DavIOVecInput *i,
                               DavIOVecOuput* o, DavixError** err){

    DavixError* tmp_err=NULL;
    dav_ssize_t ret;
    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest::copyChunk copy {} bytes with offset {}", i->diov_size, i->diov_offset);
    // if size ==0, request set to 1 byte due to server behavior, read the stupid byte and skip
    if( i->diov_size ==0){
        char trash[2];
        if( (ret = req.readSegment(trash, 1, &tmp_err)) > 0){
            o->diov_buffer = i->diov_buffer;
            o->diov_size = 0;
            ret = 0;
        }

    } else if( ( ret = req.readSegment((char*)i->diov_buffer, i->diov_size, &tmp_err)) >0){
        o->diov_buffer = i->diov_buffer;
        o->diov_size = ret;
    }
    if(tmp_err){
        DavixError::propagateError(err, tmp_err);
    }else{
        DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest::copyChunk {} bytes copied with success",ret);
    }
    return ret;
}


dav_ssize_t HttpIOVecOps::parseMultipartRequest(HttpRequest & _req,
                                            const DavIOVecInput *input_vec,
                                            DavIOVecOuput * output_vec,
                                            const dav_size_t count_vec, DavixError** err){
    std::string boundary;
    dav_ssize_t ret = 0, tmp_ret =0;
    dav_size_t off=0;
    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest multi part parsing");

    if(get_multi_part_info(_req, boundary, err)  != 0 ){
        DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, "Invalid Header Content info for multi part request");
        return -1;
    }
    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest multi-part boundary {}", boundary);

    while(off < count_vec){
       DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest try to find chunk offset:{} size {}", input_vec[off].diov_offset, input_vec[off].diov_size);
       ChunkInfo infos;
       int n_try = 0;
       if( (tmp_ret = parse_multi_part_header(_req, boundary, infos,
                                     n_try, err)) < 0){
            return -1;
       }

       if(infos.offset == 0 &&  infos.size == 0 && infos.bounded == true){
            DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest multi-part : end of the request found {} chunks treated on {}", off, count_vec);
            return ret;
       }

       if( input_vec[off].diov_size !=0 &&
               (infos.offset != input_vec[off].diov_offset
               || infos.size != input_vec[off].diov_size )){
            HttpIoVecSetupErrorMultiPartSize(err,
                                            input_vec[off].diov_offset, input_vec[off].diov_size,
                                             infos.offset, infos.size);
            return -1;
       }

       if( (tmp_ret = copyChunk(_req, &input_vec[off], &output_vec[off], err)) <0 )
           return -1;

       ret += tmp_ret;
       DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest chunk parsed with success, next chunk..");
       off++;
    }

    // finish with success, dump the remaining part of the query to end the request properly
    char buffer[255];
    while( _req.readBlock(buffer, 255, NULL) > 0);


    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, "Davix::parseMultipartRequest end {} {}", off, count_vec);
    return ret;
}


bool is_a_start_boundary_part(char* buffer, dav_size_t s_buff, const std::string & boundary,
                            DavixError** err){
    if(s_buff > 3){
        char * p = buffer;
        if( *p == '-' && *(p+1)== '-'){
            if( strcmp(buffer+2, boundary.c_str()) ==0){
                return true;
            }
        }
    }
    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, "Invalid boundary delimitation");
    HttpIoVecSetupErrorMultiPart(err);
    return false;
}





struct ElemChunk{
    ElemChunk(const DavIOVecInput* in, DavIOVecOuput* ou) :
        _in(in),
        _ou(ou),
        _cursor((char*) in->diov_buffer){
        _ou->diov_size=0; // reset elem read status
        _ou->diov_buffer = _in->diov_buffer;
    }

    const DavIOVecInput *_in;
    DavIOVecOuput * _ou;
    char *_cursor;

};

typedef std::pair<dav_off_t, ElemChunk> PairChunk;


typedef std::multimap<dav_off_t, ElemChunk> MapChunk;


// order the chunk by offset
static void fill_map_chunk(MapChunk & m, const DavIOVecInput *input_vec,
                                    DavIOVecOuput * output_vec,
                                    const dav_size_t count_vec){
    for(dav_size_t s = 0; s < count_vec; s++){
        m.insert(PairChunk(input_vec[s].diov_offset, ElemChunk(&input_vec[s], &output_vec[s])));
    }
}


static void balance_iterator_windows(MapChunk & m,
                                     MapChunk::iterator & start, MapChunk::iterator & end,
                                     dav_ssize_t pos, dav_ssize_t read_size){
    dav_ssize_t size_part;
    dav_off_t off_part;
    for(;start != m.end();){ // move the it to first concerned block
        size_part = (*start).second._in->diov_size;
        off_part = (*start).second._in->diov_offset;
        if(pos > ((dav_ssize_t)off_part) + size_part)
            start++;
        else
            break;
    }

    const dav_ssize_t end_chunk_pos = pos + read_size;
    for(;end != m.end();){
        off_part = (*end).second._in->diov_offset;
        if(end_chunk_pos > (dav_ssize_t)off_part )
            end++;
        else
            break;
    }

}

static void fill_concerned_chunk_buffer(MapChunk & m,
                                        MapChunk::iterator & start, MapChunk::iterator & end,
                                        char* buffer, dav_ssize_t read_size, dav_ssize_t pos){
    (void) m;
    for(MapChunk::iterator it = start; it != end; it++){
        const dav_ssize_t size_part = (*it).second._in->diov_size;
        const dav_off_t off_part = (*it).second._in->diov_offset;
        const dav_ssize_t cur_chunk_size = (*it).second._ou->diov_size;
        const char* p_buff = (char*) (*it).second._ou->diov_buffer;

        const dav_ssize_t current_chunk_offset = ((dav_ssize_t) off_part + cur_chunk_size);
        const dav_ssize_t read_offset =  current_chunk_offset - pos;
        const dav_ssize_t s_needed = std::min(size_part - cur_chunk_size, read_size - read_offset);
        if(s_needed > 0){
            memcpy((void*) (p_buff + cur_chunk_size), buffer+ read_offset, s_needed);
            (*it).second._ou->diov_size += s_needed;
        }
    }
}

static dav_ssize_t sum_all_chunk_size(const MapChunk & cmap){

    dav_ssize_t res =0;
    for(MapChunk::const_iterator it = cmap.begin(); it != cmap.end(); ++it){
        res += (*it).second._ou->diov_size;
    }
    return res;
}

dav_ssize_t HttpIOVecOps::simulateMultiPartRequest(HttpRequest & _req, const DavIOVecInput *input_vec,
                                 DavIOVecOuput * output_vec,
                   const dav_size_t count_vec, DavixError** err){
    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, " -> Davix vec : 200 full file, simulate vec io");
    MapChunk cmap;
    dav_ssize_t total_read_size=0, tmp_read_size;
    char buffer[DAVIX_READ_BLOCK_SIZE];

    fill_map_chunk(cmap, input_vec, output_vec, count_vec);
    MapChunk::iterator it_start=cmap.begin(),it_end = cmap.begin();
    while( (tmp_read_size = _req.readBlock(buffer, DAVIX_READ_BLOCK_SIZE, err)) >0){
        balance_iterator_windows(cmap, it_start, it_end, total_read_size, tmp_read_size); // re-balance the interested windows
        fill_concerned_chunk_buffer(cmap, it_start, it_end, buffer, tmp_read_size, total_read_size); // fill the interested window
        total_read_size += tmp_read_size;
    }
    if(tmp_read_size < 0)
        return -1;

    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CHAIN, " <- Davix vec : 200 full file, simulate vec io");
    return sum_all_chunk_size(cmap);
}



} // Davix
