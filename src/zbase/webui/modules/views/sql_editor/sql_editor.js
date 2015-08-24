ZBase.registerView((function() {

  var query_mgr;

  var init = function(params) {
    query_mgr = EventSourceHandler();
    render(params.path);
  };

  var destroy = function() {
    query_mgr.closeAll();
  };

  var render = function(path) {
    if (path == "/a/sql") {
      displayQueryListView();
    } else {
      var path_prefix = "/a/sql/"
      displayQueryEditor(path.substr(path_prefix.length));
    }
  };

  var displayQueryListView = function() {
    $.showLoader();
    $.httpGet("/api/v1/documents", function(r) {
      if (r.status == 200) {
        var documents = JSON.parse(r.response).documents;
        renderQueryListView(documents);
      } else {
        $.fatalError();
      }
    });
  }

  var displayQueryEditor = function(query_id) {
    $.httpGet("/api/v1/documents/sql_queries/" + query_id, function(r) {
      if (r.status == 200) {
        var doc = JSON.parse(r.response);
        renderQueryEditorView(doc);
      } else {
        $.fatalError();
      }
    });
  };

  var createNewQuery = function() {
    $.httpPost("/api/v1/documents/sql_queries", "", function(r) {
      if (r.status == 201) {
        var response = JSON.parse(r.response);
        $.navigateTo("/a/sql/" + response.uuid);
        return;
      } else {
        $.fatalError();
      }
    });
  };

  var executeQuery = function(e) {
    showQueryLoader();

    var query_string = e.value;
    var query = query_mgr.get(
      "sql_query",
      "/api/v1/sql_stream?query=" + encodeURIComponent(query_string));

    query.addEventListener('result', function(e) {
      query_mgr.close("sql_query");

      var data = JSON.parse(e.data);
      renderQueryResult(data.results);
      hideQueryLoader();
    });

    query.addEventListener('error', function(e) {
      query_mgr.close("sql_query");

      try {
        renderQueryError(JSON.parse(e.data).error);
      } catch (e) {
        renderQueryError(e.data);
      }

      hideQueryLoader();
    });
  }

  var renderQueryListView = function(documents) {
    var page = $.getTemplate(
        "views/sql_editor",
        "zbase_sql_editor_overview_main_tpl");

    renderDocumentsList(
        page.querySelector(".zbase_sql_editor_overview tbody"),
        documents);

    $.onClick(
        page.querySelector("button[data-action='new-query']"),
        createNewQuery);

    $.handleLinks(page);
    $.replaceViewport(page)
    $.hideLoader();
  };

  var renderDocumentsList = function(tbody_elem, documents) {
    documents.forEach(function(doc) {
      var url = "/a/sql/" + doc.uuid;
      var tr = document.createElement("tr");
      tr.innerHTML = 
          "<td><a href='" + url + "'>" + doc.name + "</a></td>" +
          "<td><a href='" + url + "'>" + doc.type + "</a></td>" +
          "<td><a href='" + url + "'>&mdash;</a></td>";

      $.onClick(tr, function() { $.navigateTo(url); });
      tbody_elem.appendChild(tr);
    });
  };

  var renderQueryEditorView = function(doc) {
    var page = $.getTemplate(
        "views/sql_editor",
        "zbase_sql_editor_main_tpl");

    // code editor
    var editor = $("z-codeeditor", page);
    editor.setValue(doc.query);
    editor.addEventListener("execute", executeQuery);

    // execute button
    $.onClick($("button[data-action='execute-query']", page), function() {
      editor.execute();
    });

    // document name + name editing
    // FIXME html escaping
    $(".zbase_sql_editor_title h2", page).innerHTML = doc.title;

    //Editor.doc_sync = new DocSync(function() {
    //  return "content=" + encodeURIComponent(document.querySelector(".zbase_sql_editor_pane fn-codeeditor").getValue()) +
    //  "&name=" + encodeURIComponent(document.querySelector(".zbase_sql_editor_pane input[name='doc_name']").value)
    //  },
    //  "/api/v1/sql_queries/" + Editor.doc_id,
    //  document.querySelector(".zbase_sql_editor_pane .zbase_sql_editor_infobar")
    //);

    $.handleLinks(page);
    $.replaceViewport(page);
  };

  var renderQueryResult = function(res) {
    console.log("render result");
  }

  var renderQueryError = function(error) {
    console.log("render error");
  }

  var showQueryLoader = function() {
    console.log("show loader");
  }

  var hideQueryLoader = function() {
    console.log("hide loader");
  }

  return {
    name: "sql_editor",
    loadView: init,
    unloadView: destroy,
    handleNavigationChange: render
  };

})());
