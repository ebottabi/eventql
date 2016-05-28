/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include "eventql/util/util/binarymessagewriter.h"
#include "eventql/transport/http/rpc_servlet.h"
#include "eventql/db/RecordEnvelope.pb.h"
#include "eventql/util/json/json.h"
#include <eventql/util/wallclock.h>
#include <eventql/util/thread/wakeup.h>
#include "eventql/util/protobuf/MessageEncoder.h"
#include "eventql/util/protobuf/MessagePrinter.h"
#include "eventql/util/protobuf/msg.h"
#include <eventql/util/util/Base64.h>
#include <eventql/util/fnv.h>
#include <eventql/io/sstable/sstablereader.h>

#include "eventql/eventql.h"

namespace eventql {

RPCServlet::RPCServlet(
    TableService* node,
    MetadataStore* metadata_store,
    const String& tmpdir) :
    node_(node),
    metadata_store_(metadata_store),
    tmpdir_(tmpdir) {}

void RPCServlet::handleHTTPRequest(
    RefPtr<http::HTTPRequestStream> req_stream,
    RefPtr<http::HTTPResponseStream> res_stream) {
  const auto& req = req_stream->request();
  URI uri(req.uri());

  logDebug("eventql", "HTTP Request: $0 $1", req.method(), req.uri());

  http::HTTPResponse res;
  res.populateFromRequest(req);

  res.addHeader("Access-Control-Allow-Origin", "*");
  res.addHeader("Access-Control-Allow-Methods", "GET, POST");
  res.addHeader("Access-Control-Allow-Headers", "X-TSDB-Namespace");

  if (req.method() == http::HTTPMessage::M_OPTIONS) {
    req_stream->readBody();
    res.setStatus(http::kStatusOK);
    res_stream->writeResponse(res);
    return;
  }

  try {
    if (uri.path() == "/tsdb/insert") {
      req_stream->readBody();
      insertRecords(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/replicate") {
      req_stream->readBody();
      replicateRecords(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/compact") {
      req_stream->readBody();
      compactPartition(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/stream") {
      req_stream->readBody();
      streamPartition(&req, &res, res_stream, &uri);
      return;
    }

    if (uri.path() == "/tsdb/partition_info") {
      req_stream->readBody();
      fetchPartitionInfo(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/sql") {
      req_stream->readBody();
      executeSQL(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/sql_stream") {
      req_stream->readBody();
      executeSQLStream(&req, &res, res_stream, &uri);
      return;
    }

    if (uri.path() == "/tsdb/update_cstable") {
      updateCSTable(uri, req_stream.get(), &res);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/rpc/store_metadata_file") {
      req_stream->readBody();
      storeMetadataFile(uri, &req, &res);
      res_stream->writeResponse(res);
      return;
    }

    res.setStatus(http::kStatusNotFound);
    res.addBody("not found");
    res_stream->writeResponse(res);
  } catch (const Exception& e) {
    logError("tsdb", e, "error while processing HTTP request");

    res.setStatus(http::kStatusInternalServerError);
    res.addBody(StringUtil::format("error: $0: $1", e.getTypeName(), e.getMessage()));
    res_stream->writeResponse(res);
  }

  res_stream->finishResponse();
}

void RPCServlet::insertRecords(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  auto record_list = msg::decode<RecordEnvelopeList>(req->body());
  node_->insertRecords(record_list);
  res->setStatus(http::kStatusCreated);
}

void RPCServlet::compactPartition(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    return;
  }

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?table=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  node_->compactPartition(
      tsdb_namespace,
      table_name,
      SHA1Hash::fromHexString(partition_key));

  res->setStatus(http::kStatusCreated);
}

void RPCServlet::replicateRecords(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  auto record_list = msg::decode<RecordEnvelopeList>(req->body());
  auto insert_flags = (uint64_t) InsertFlags::REPLICATED_WRITE;
  if (record_list.sync_commit()) {
    insert_flags |= (uint64_t) InsertFlags::SYNC_COMMIT;
  }

  node_->insertRecords(record_list, insert_flags);
  res->setStatus(http::kStatusCreated);
}

void RPCServlet::streamPartition(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  String table_name;
  if (!URI::getParam(params, "stream", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?stream=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  size_t sample_mod = 0;
  size_t sample_idx = 0;
  String sample_str;
  if (URI::getParam(params, "sample", &sample_str)) {
    auto parts = StringUtil::split(sample_str, ":");

    if (parts.size() != 2) {
      res->setStatus(http::kStatusBadRequest);
      res->addBody("invalid ?sample=... parameter, format is <mod>:<idx>");
      res_stream->writeResponse(*res);
    }

    sample_mod = std::stoull(parts[0]);
    sample_idx = std::stoull(parts[1]);
  }

  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "application/octet-stream");
  res->addHeader("Connection", "close");
  res_stream->startResponse(*res);

  node_->fetchPartitionWithSampling(
      tsdb_namespace,
      table_name,
      SHA1Hash::fromHexString(partition_key),
      sample_mod,
      sample_idx,
      [&res_stream] (const Buffer& record) {
    util::BinaryMessageWriter buf;

    if (record.size() > 0) {
      buf.appendUInt64(record.size());
      buf.append(record.data(), record.size());
      res_stream->writeBodyChunk(Buffer(buf.data(), buf.size()));
    }

    res_stream->waitForReader();
  });

  util::BinaryMessageWriter buf;
  buf.appendUInt64(0);
  res_stream->writeBodyChunk(Buffer(buf.data(), buf.size()));

  res_stream->finishResponse();
}

void RPCServlet::fetchPartitionInfo(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    return;
  }

  String table_name;
  if (!URI::getParam(params, "stream", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?stream=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  auto pinfo = node_->partitionInfo(
      tsdb_namespace,
      table_name,
      SHA1Hash::fromHexString(partition_key));

  if (pinfo.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
  } else {
    res->setStatus(http::kStatusOK);
    res->addHeader("Content-Type", "application/x-protobuf");
    res->addBody(*msg::encode(pinfo.get()));
  }
}

void RPCServlet::executeSQL(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  auto tsdb_namespace = req->getHeader("X-TSDB-Namespace");
  auto query = req->body().toString();

  Buffer result;
  //node_->sqlEngine()->executeQuery(
  //    tsdb_namespace,
  //    query,
  //    new csql::ASCIITableFormat(BufferOutputStream::fromBuffer(&result)));

  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "text/plain");
  res->addHeader("Connection", "close");
  res->addBody(result);
}

void RPCServlet::executeSQLStream(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream,
    URI* uri) {
  http::HTTPSSEStream sse_stream(res, res_stream);
  sse_stream.start();

  try {
    const auto& params = uri->queryParams();

    String tsdb_namespace;
    if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
      RAISE(kRuntimeError, "missing ?namespace=... parameter");
    }

    String query;
    if (!URI::getParam(params, "query", &query)) {
      RAISE(kRuntimeError, "missing ?query=... parameter");
    }

    //node_->sqlEngine()->executeQuery(
    //    tsdb_namespace,
    //    query,
    //    new csql::JSONSSEStreamFormat(&sse_stream));

  } catch (const StandardException& e) {
    logError("sql", e, "SQL execution failed");

    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
    json.beginObject();
    json.addObjectEntry("error");
    json.addString(e.what());
    json.endObject();

    sse_stream.sendEvent(buf, Some(String("error")));
  }

  sse_stream.finish();
}

void RPCServlet::updateCSTable(
    const URI& uri,
    http::HTTPRequestStream* req_stream,
    http::HTTPResponse* res) {
  const auto& params = uri.queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    RAISE(kRuntimeError, "missing ?namespace=... parameter");
  }

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?table=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  String version;
  if (!URI::getParam(params, "version", &version)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?version=... parameter");
    return;
  }

  auto tmpfile_path = FileUtil::joinPaths(
      tmpdir_,
      StringUtil::format("upload_$0.tmp", Random::singleton()->hex128()));

  {
    auto tmpfile = File::openFile(
        tmpfile_path,
        File::O_CREATE | File::O_READ | File::O_WRITE);

    req_stream->readBody([&tmpfile] (const void* data, size_t size) {
      tmpfile.write(data, size);
    });
  }

  node_->updatePartitionCSTable(
      tsdb_namespace,
      table_name,
      SHA1Hash::fromHexString(partition_key),
      tmpfile_path,
      std::stoull(version));

  res->setStatus(http::kStatusCreated);
}

void RPCServlet::storeMetadataFile(
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  const auto& params = uri.queryParams();

  String db_namespace;
  if (!URI::getParam(params, "namespace", &db_namespace)) {
    RAISE(kRuntimeError, "missing ?namespace=... parameter");
  }

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?table=... parameter");
    return;
  }

  String txid;
  if (!URI::getParam(params, "txid", &txid)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?txid=... parameter");
    return;
  }

  MetadataFile metadata_file;
  {
    auto is = req->getBodyInputStream();
    auto rc = metadata_file.decode(is.get());
    if (!rc.isSuccess()) {
      res->setStatus(http::kStatusInternalServerError);
      res->addBody("ERROR: " + rc.message());
      return;
    }
  }

  {
    auto rc = metadata_store_->storeMetadataFile(
        db_namespace,
        table_name,
        SHA1Hash::fromHexString(txid),
        metadata_file);

    if (!rc.isSuccess()) {
      res->setStatus(http::kStatusInternalServerError);
      res->addBody("ERROR: " + rc.message());
      return;
    }
  }

  res->setStatus(http::kStatusCreated);
}

}

