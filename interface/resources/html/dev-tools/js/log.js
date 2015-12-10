$(function(){

    var testLog = [
        "[12/09 15:58:04] [WARNING] Could not attach to shared memory at key \"domain-server.local-port\"",
        "[12/09 15:58:04] [DEBUG] Clearing the NodeList. Deleting all nodes in list."
    ]

    var sanitizedLog = [];

    // enumerate the current log entries and set them up for DataTables
    $.each(Developer.log, function(index, message) {
        // pull out the time from the log entry
        var timeStart = message.indexOf('[') + 1;
        var timeEnd = message.indexOf(']');
        var time = message.substring(timeStart, timeEnd);

        // pull out the type from the log entry
        var typeStart = message.indexOf('[', timeStart + 1) + 1;
        var typeEnd = message.indexOf(']', timeEnd + 1);
        var type = message.substring(typeStart, typeEnd);

        sanitizedLog.push([time, type, message.substring(typeEnd + 1)]);
    });

    // create a DataTable with the current log entries
    $("#log").DataTable({
        data: sanitizedLog,
        columns: [
            { title: "Time" },
            { title: "Type" },
            { title: "Message" }
        ],
        bPaginate: false,
        ordering: false
    });
})
