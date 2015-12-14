$(function(){
    function sanitizedMessageArray(index, message) {
        // pull out the time from the log entry
        var timeStart = message.indexOf('[') + 1;
        var timeEnd = message.indexOf(']');
        var time = message.substring(timeStart, timeEnd);

        // pull out the type from the log entry
        var typeStart = message.indexOf('[', timeStart + 1) + 1;
        var typeEnd = message.indexOf(']', timeEnd + 1);
        var type = message.substring(typeStart, typeEnd);

        return [time, type, message.substring(typeEnd + 1)];
    }

    function contextualClassForType(messageType) {
        switch(messageType) {
            case "DEBUG":
            case "SUPPRESS":
                return "success";
            case "INFO":
                return "info";
            case "WARNING":
                return "warning";
            case "FATAL":
            case "CRITICAL":
                return "danger";
        }
    }

    var FILTERED_CLASS = "filtered"
    var HIDDEN_DEBUG_CLASS = "hidden-debug"

    var tableRows = 0;
    var filter = "";
    var hideDebug = true;
    var stickToBottom = false;

    function addRowToTable(index, message) {
        var messageArray = sanitizedMessageArray(index, message);
        var row = $("<tr/>");

        $.each(messageArray, function(index, column){
            var cell = $("<td>" + column + "</td>");

            if (index == 1) {
                // add the message type as a class to this cell
                cell.addClass(contextualClassForType(messageArray[1]));
            }

            row.append(cell);
        });

        // add the message type as data to this row
        row.attr("data-message-type", messageArray[1]);

        // check if this row should be filtered immediately
        if (row.text().toLowerCase().indexOf(filter) == -1) {
            row.addClass(FILTERED_CLASS);
        }

        // check if this is debug that should be filtered immediately
        if (hideDebug && messageArray[1] == "DEBUG") {
            row.addClass(HIDDEN_DEBUG_CLASS);
        }

        // append the row to the table
        $('table tbody').append(row);

        ++tableRows;
    }

    // connect to the web channel that we set up on the C++ side
    var baseUrl = (/[?&]webChannelBaseURL=([A-Za-z0-9\-:/\.]+)/.exec(location.search)[1]);
    console.log("Connecting to WebSocket server at " + baseUrl + ".");
    var socket = new WebSocket(baseUrl);

    socket.onclose = function() {
        console.error("WebSocket closed");
    };
    socket.onerror = function(error) {
        console.error("WebSocket error: " + error);
    };

    socket.onopen = function() {
        output("WebSocket connected, setting up QWebChannel.");
        new QWebChannel(socket, function(channel) {
            // when we get a new log entry, sanitize it and add it to the table
            channel.developer.newLogLine.connect(function(index, message){
                if (index >= tableRows) {
                    addRowToTable(index, message);

                    if (stickToBottom) {
                        // we're in stick to bottom mode, jump to bottom of window
                        $(window).scrollTop($(document).height());
                    }
                }
            });

            // enumerate the current log entries and set them up for DataTables
            $.each(channel.developer.log, function(index, message) {
                addRowToTable(index, message);
            });

            // handle reveal of log file on button click
            $('#reveal-log-btn').click(function(){
                channel.developer.revealLogFile();
            });
        }
    }

    // change the column filter if the user asks for verbose debug
    $('#hide-debug-checkbox').change(function(){
        if (this.checked) {
            // hide the debug output in the table
            hideDebug = true;
            $('tr[data-message-type="DEBUG"]').addClass(HIDDEN_DEBUG_CLASS);
        } else {
            // show the debug output in the table
            hideDebug = false;
            $('tr[data-message-type="DEBUG"]').removeClass(HIDDEN_DEBUG_CLASS);
        }
    });

    // fire the change event on the verbose debug checkbox to start with the right value
    $('#hide -debug-checkbox').change();

    // handle filtering of table rows on input change
    $('#search-input').on('input', function(){
        filter = $(this).val().toLowerCase();
        $('tbody tr').each(function(){
            // decide to hide or show the row if it matches the filter
            if (filter && $(this).text().toLowerCase().indexOf(filter) == -1) {
                $(this).addClass(FILTERED_CLASS);
            } else {
                $(this).removeClass(FILTERED_CLASS);
            }
        });
    });

    $(window).scroll(function() {
        if ($(window).scrollTop() + $(window).height() == $(document).height()) {
            // user has hit the bottom, stick the scroll to incoming log entries
            stickToBottom = true;
        } else {
            // user scrolled off the bottom, don't force log to stick to bottom
            stickToBottom = false;
        }
    });
})
