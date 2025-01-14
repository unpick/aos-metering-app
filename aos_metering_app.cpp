// vim: sw=4 expandtab
// Copyright (c) Aetheros, Inc.  See COPYRIGHT

// Example AOS meter reading and reporting app, designed to work with the MeterSummary.py example from sdk-python to demonstrate
// bidirectional, end-to-end communications.

#include <aos/AppMain.hpp>
#include <aos/Log.hpp>
#include <m2m/AppEntity.hpp>
#include <xsd/m2m/Names.hpp>

#include <thread>
#include <queue>
#include <fstream>

#include <sys/types.h>
#include <unistd.h>

#include "meter.h"

using namespace std::chrono;
using namespace nlohmann;
using namespace Meter;


///////////////// Configure the following for your environment /////////////////

const std::string APP_RESOURCE = "meterSummary";                // Public name of this app; NOTE Must be unique across all apps
const std::string APP_NAME = "com.grid-net.meterdemo";          // Name of the IN-AE to report to

////////////////////////////// End of site config //////////////////////////////


const aos::LogLevel LOG_LEVEL = aos::LogLevel::LOG_INFO;        // Maximum logging level; set to LOG_DEBUG for more logread detail
const std::string IN_CSE = "/PN_CSE";                           // Absolute path to the IN-CSE
const std::string IN_AE_RESOURCE_NAME = "";                     // Report content instance name; empty string for automatic names
const std::string APP_PATH = "./" + APP_RESOURCE;               // Relative path of our local configuration container
const int MAX_INSTANCE_AGE_S = 900;                             // Duration to create containers for
const int BACKOFF_DEFAULT_S = 30;
const int SAMPLE_PERIOD_DEFAULT = 1;                            // Time between information requests from the mtrsvc, in seconds
const int REPORT_PERIOD_DEFAULT = 3600;                         // Time between meter information reports to the IN-AE, in seconds
const bool SPOOF_METER = false;                                 // Set to true if using metersim

// Member objects
m2m::AppEntity appEntity;                                       // OneM2M Application Entity (AE) object
Report report;                                                  // mtrsvc sample accumulator and reporter
int reportPeriod = REPORT_PERIOD_DEFAULT;
milliseconds reportTime = milliseconds(0);                      // Scheduled time to transmit the next report

std::queue<SampleSummary> reportQueue;                          // Queue to pass report summaries to the report summary thread

std::string containerPath;                                      // Path to IN-AE's container that we will create reports in

// Function prototypes
void spawn_threads();
void report_queue_thread();

void spoofSample(Sample& sample);
std::string singleQuoteToDoubleQuote(const std::string& s);

bool create_subscription(const std::string& parentPath, const std::string& resourceName);
bool create_meter_read_policy();
bool discover_in_ae(const std::string& csePath, const std::string& appName, std::string& path);
bool discover_container(const std::string& parentPath, std::string& containerPath);
bool create_container(const std::string& parentPath, const std::string& resourceName, const int maxInstanceAge);
bool discover_content_instances(const std::string& parentPath);
bool create_content_instance(const std::string& parentPath, const std::string& resourceName, const SampleSummary& sampleSummary);
bool delete_content_instance(const std::string& path);
void notificationCallback(m2m::Notification notification);
void parseReportInterval(const int seconds);
void parseMeterSvcData(const xsd::mtrsvc::MeterSvcData& meterSvcData);

// Helper functions
[[noreturn]] void usage(const char *prog)
{
    fprintf(stderr, "Usage %s [-d] [-p <POA-URI>|-a <IPADDR[:PORT]>] [CSE-URI]\n", prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    // scope the application main loop
    aos::AppMain appMain;
    const char *poaUri = nullptr;
    const char *poaAddr = nullptr;

    aos::setLogLevel(LOG_LEVEL);

    int opt;
    while ((opt = getopt(argc, argv, "dp:a:")) != -1)
    {
        switch (opt)
        {
        case 'd':
            aos::setLogLevel(aos::LogLevel::LOG_DEBUG);
            break;
        case 'p':
            poaUri = optarg;
            break;
        case 'a':
            poaAddr = optarg;
            break;
        default:
            usage(argv[0]);
            break;
        }
    }

    if (argc - optind > 1)
    {
        usage(argv[0]);
    }

    // initialize the AE object
    appEntity = m2m::AppEntity(notificationCallback);

    if (argc - optind == 1)
    {
        appEntity.setCseUri(argv[optind]);
    }

    if (poaUri)
    {
        appEntity.setPoaUri(poaUri);
    }
    else if (poaAddr)
    {
        appEntity.setPoaAddr(poaAddr);
    }

    spawn_threads();

    while (true)
    {
        seconds backoffSeconds{BACKOFF_DEFAULT_S};

        // activate the AE (checks registration, registers if necessary, updates poa if necessary)
        logInfo("activating");
        while (!appEntity.activate())
        {
            auto failureReason = appEntity.getActivationFailureReason();
            logError("activation failed: reason: " << failureReason);
            if (failureReason != m2m::ActivationFailureReason::Timeout
                && failureReason != m2m::ActivationFailureReason::NotRegistered)
            {
                logInfo("retrying in " << backoffSeconds.count() << " seconds");
                std::this_thread::sleep_for(backoffSeconds);
                if (backoffSeconds < minutes{16})
                    backoffSeconds *= 2;
            }
        }

        logInfo("activated");
        if (!create_subscription("./metersvc/reads", APP_RESOURCE + "-sub-01"))
        {
            logError("subscription creation failed");
            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
            continue;
        }

        if (!create_meter_read_policy())
        {
            logError("meter read policy creation failed");
            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
            continue;
        }

        // Example: Discover the MN-CSE's containers, to a maximum depth of 2.
//        std::string dummy;
//        if (!discover_container(".", dummy))
//        {
//            logError("Local container discovery failed");
//            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
//            continue;
//        }

        // Example: Discover the content instances within a given local container.
//        discover_content_instances("./metersvc/policies");

        // Create the local container for receiving remote configuration commands.
        if (!create_container(".", APP_RESOURCE, MAX_INSTANCE_AGE_S))
        {
            logError("Container creation failed");
            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
            continue;
        }

        // Subscribe to newly created content instances in the local configuration container.
        if (!create_subscription(APP_PATH, APP_RESOURCE + "-sub-01"))
        {
            logError("Local subscription creation failed");
            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
            continue;
        }

        std::string aePath;
        if (!discover_in_ae(IN_CSE, APP_NAME, aePath))
        {
            logError("IN-AE discovery failed");
            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
            continue;
        }

        if (!discover_container(aePath, containerPath))
        {
            logError("IN-AE container discovery failed");
            std::this_thread::sleep_for(seconds{BACKOFF_DEFAULT_S});
            continue;
        }

        break;
    }

    appEntity.waitForever();
}

// Spawn the background thread(s) used to perform CoAP requests initiated from the message handler's context.
void spawn_threads()
{
    logDebug("Spawning meter summary publishing thread ...");
    std::thread meter_summary_publishing_thread(report_queue_thread);
    meter_summary_publishing_thread.detach();
    logDebug("Spawned meter summary publishing thread");
}

// Thread to report sample summaries passed in via reportQueue.
void report_queue_thread()
{
    while (true)
    {
        if (reportQueue.empty())
        {
            std::this_thread::sleep_for(seconds{1});
            continue;
        }

        auto summary = reportQueue.front();
        reportQueue.pop();
        logDebug("Sending summary of " << summary.count << " samples");
        if (!create_content_instance(containerPath, IN_AE_RESOURCE_NAME, summary))
            logError("Failed to send summary");
    }
}

// Create a sample with a minimal set of spoofed data.
void spoofSample(Sample& sample)
{
    sample.p1.vrms = EXPECTED_VOLTAGE;
    sample.frequency = EXPECTED_FREQUENCY;
    logDebug("Spoofed " << sample.p1.vrms << " V at " << sample.frequency << " Hz");
}

// Take a string, convert all single quotes to double quotes, and return the result.
std::string singleQuoteToDoubleQuote(const std::string& s)
{
    std::string result;
    for (char c : s)
    {
        if (c == '\'')
            result += '"';
        else
            result += c;
    }
    return result;
}

// Create a subscription with the given name to content instances created in the given parent path.
bool create_subscription(const std::string& parentPath, const std::string& resourceName)
{
    xsd::m2m::Subscription subscription = xsd::m2m::Subscription::Create();

    subscription.creator = std::string();

    // set the subscription's name
    subscription.resourceName = resourceName;

    // have all resource attributes be provided in the notifications
    subscription.notificationContentType = xsd::m2m::NotificationContentType::all_attributes;

    // set the notification destination to be this AE.
    subscription.notificationURI = xsd::m2m::ListOfURIs();
    subscription.notificationURI->push_back(appEntity.getResourceId());

    // set eventNotificationCriteria to creation and deletion of child resources
    xsd::m2m::EventNotificationCriteria eventNotificationCriteria;
    eventNotificationCriteria.notificationEventType.assign().push_back(xsd::m2m::NotificationEventType::Create_of_Direct_Child_Resource);
    //eventNotificationCriteria.notificationEventType.push_back(xsd::m2m::NotificationEventType::Delete_of_Direct_Child_Resource);
    subscription.eventNotificationCriteria = std::move(eventNotificationCriteria);

    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Create, m2m::To{parentPath});
    request.req->resultContent = xsd::m2m::ResultContent::Nothing;
    request.req->resourceType = xsd::m2m::ResourceType::subscription;
    request.req->primitiveContent = xsd::toAnyNamed(subscription);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    logInfo("Subscription: " << toString(response->responseStatusCode));

    return (response->responseStatusCode == xsd::m2m::ResponseStatusCode::CREATED
            || response->responseStatusCode == xsd::m2m::ResponseStatusCode::CONFLICT);
}

// Create the default mtrsvc read policy.
// NOTE This persists until the device is restarted, even if the app is stopped.
bool create_meter_read_policy()
{
    xsd::mtrsvc::ScheduleInterval scheduleInterval;
    scheduleInterval.end = nullptr;
    scheduleInterval.start = "2020-06-19T00:00:00";

    xsd::mtrsvc::TimeSchedule timeSchedule;
    timeSchedule.recurrencePeriod = SAMPLE_PERIOD_DEFAULT;
    timeSchedule.scheduleInterval = std::move(scheduleInterval);

    xsd::mtrsvc::MeterReadSchedule meterReadSchedule;
    meterReadSchedule.readingType = "powerQuality";
    meterReadSchedule.timeSchedule = std::move(timeSchedule);

    xsd::mtrsvc::MeterServicePolicy meterServicePolicy;
    meterServicePolicy.meterReadSchedule = std::move(meterReadSchedule);

    xsd::m2m::ContentInstance policyInst = xsd::m2m::ContentInstance::Create();
    policyInst.content = xsd::toAnyTypeUnnamed(meterServicePolicy);

    policyInst.resourceName = "metersvc-" + APP_RESOURCE;

    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Create, m2m::To{"./metersvc/policies"});
    request.req->resultContent = xsd::m2m::ResultContent::Nothing;
    request.req->resourceType = xsd::m2m::ResourceType::contentInstance;
    request.req->primitiveContent = xsd::toAnyNamed(policyInst);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    logInfo("Policy creation: " << toString(response->responseStatusCode));

    return (response->responseStatusCode == xsd::m2m::ResponseStatusCode::CREATED
            || response->responseStatusCode == xsd::m2m::ResponseStatusCode::CONFLICT);
}

// Discover the given CSE's AE's matching appName, and return the first (presumed to be the most recently created) via aePath.
bool discover_in_ae(const std::string& csePath, const std::string& appName, std::string& aePath)
{
    aePath = "";

    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Retrieve, m2m::To{csePath});

    xsd::m2m::FilterCriteria fc;
    fc.filterUsage = xsd::m2m::FilterUsage::Discovery;
    fc.resourceType = xsd::m2m::ResourceTypeList();
    fc.resourceType->push_back(xsd::m2m::ResourceType::AE);
    fc.level = 1;

    xsd::m2m::Attribute attr;
    attr.name = xsd::m2m::sn_appName;
    attr.value = xsd::toAnyTypeUnnamed(appName);                // NOTE Argument must be std::string, not char*

    auto &attrs = fc.attribute.assign();
    attrs.emplace_back(std::move(attr));

    request.req->filterCriteria = std::move(fc);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    if (response->responseStatusCode != xsd::m2m::ResponseStatusCode::OK)
    {
        logWarn("AE discovery failed: " << toString(response->responseStatusCode));
        return false;
    }

    auto json_str = xsd::toAnyTypeUnnamed(response->primitiveContent).dumpJson();
    for (int i = 0; i < json_str.length(); i += 150)
        logDebug("AE discovery [" << i << "]: " << json_str.substr(i, i + 150));

    try
    {
        auto json = nlohmann::json::parse(json_str);
        if (json.at("m2m:uril").size() < 1)
        {
            logWarn("Could not find an AE with appName " + appName);
            return false;
        }

        aePath = json.at("m2m:uril")[0].get<std::string>();

        return true;
    }
    catch (const std::exception& e)
    {
        logError("Failed to parse discovered IN-AE: " << e.what());
        return false;
    }
}

// Discover the containers within the parentPath, and return the first via containerPath.
bool discover_container(const std::string& parentPath, std::string& containerPath)
{
    containerPath = "";

    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Retrieve, m2m::To{parentPath});

    xsd::m2m::FilterCriteria fc;
    fc.filterUsage = xsd::m2m::FilterUsage::Discovery;
    fc.resourceType = xsd::m2m::ResourceTypeList();
    fc.resourceType->push_back(xsd::m2m::ResourceType::container);
    fc.level = 2;

    request.req->filterCriteria = std::move(fc);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    if (response->responseStatusCode != xsd::m2m::ResponseStatusCode::OK)
    {
        logWarn("Container discovery failed: " << toString(response->responseStatusCode));
        return false;
    }

    auto json_str = xsd::toAnyTypeUnnamed(response->primitiveContent).dumpJson();
    for (int i = 0; i < json_str.length(); i += 150)
        logDebug("Container discovery [" << i << "]: " << json_str.substr(i, i + 150));

    try
    {
        auto json = nlohmann::json::parse(json_str);
        containerPath = json.at("m2m:uril")[0].get<std::string>();
        if (containerPath.length() < 1)
        {
            logWarn("Container not found");
            return false;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        logError("Failed to parse discovered container: " << e.what());
        return false;
    }
}

// Create a container of the given name and with the given expiry age (in seconds) in the given parent path.
bool create_container(const std::string& parentPath, const std::string& resourceName, const int maxInstanceAge)
{
    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Create, m2m::To{parentPath});
    request.req->resourceType = xsd::m2m::ResourceType::container;
    request.req->resultContent = xsd::m2m::ResultContent::Hierarchical_Address;
    request.req->eventCategory = (int)xsd::m2m::StdEventCats::Immediate;        // NOTE Mandatory cast to int

    xsd::m2m::Container cnt = xsd::m2m::Container::Create();
    cnt.resourceName = resourceName;
    // When content instances are created without specifying a name, the CSE gives them unique names.  Combined with a limit on the
    // maximum number of content instances in the container, this results in a queue of content instances, which can be accessed via
    // the "latest" and "oldest" reserved names.  This is the mechanism we use, with a queue depth of 1.
    // Note that this will not work if the content instances are all created with the same name, as the CSE will treat a subsequent
    // content instance with the same name as a conflict and return a 405 error.
    cnt.maxNrOfInstances = 1;
    cnt.maxInstanceAge = maxInstanceAge;

    request.req->primitiveContent = xsd::toAnyNamed(cnt);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    if (response->responseStatusCode != xsd::m2m::ResponseStatusCode::CREATED
        && response->responseStatusCode != xsd::m2m::ResponseStatusCode::CONFLICT)
    {
        logWarn("Container creation failed: " << toString(response->responseStatusCode));
        return false;
    }

    if (response->primitiveContent.empty())
    {
        logInfo("Container creation: " << toString(response->responseStatusCode));
    }
    else
    {
        auto json = xsd::toAnyTypeUnnamed(response->primitiveContent).dumpJson();
        logInfo("Container creation: " << toString(response->responseStatusCode) << ", " << json);
    }

    return true;
}

// Discover the content instances within the parentPath.
bool discover_content_instances(const std::string& parentPath)
{
    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Retrieve, m2m::To{parentPath});

    xsd::m2m::FilterCriteria fc;
    fc.filterUsage = xsd::m2m::FilterUsage::Discovery;
    fc.resourceType = xsd::m2m::ResourceTypeList();
    fc.resourceType->push_back(xsd::m2m::ResourceType::contentInstance);
    fc.level = 1;                                               // Limit recursion depth to just immediate children

    request.req->filterCriteria = std::move(fc);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    if (response->responseStatusCode != xsd::m2m::ResponseStatusCode::OK)
    {
        logWarn("Meter read policy failed: " << toString(response->responseStatusCode));
        return false;
    }

    auto json = xsd::toAnyTypeUnnamed(response->primitiveContent).dumpJson();
    logInfo("Meter read policy: " << toString(response->responseStatusCode) << ", " << json);

    return true;
}

// Create a SampleSummary content instance of the given name in the given parent path.
bool create_content_instance(const std::string& parentPath, const std::string& resourceName, const SampleSummary& sampleSummary)
{
    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Create, m2m::To{parentPath});
    request.req->resourceType = xsd::m2m::ResourceType::contentInstance;
    request.req->resultContent = xsd::m2m::ResultContent::Hierarchical_Address;
    request.req->eventCategory = (int)xsd::m2m::StdEventCats::Immediate;        // NOTE Mandatory cast to int

    xsd::m2m::ContentInstance cin = xsd::m2m::ContentInstance::Create();
    // Set the conten instance's name, if provided; otherwise a unique one will be allocated by the CSE.
    if (resourceName != "")
        cin.resourceName = resourceName;

    ordered_json json;
    sampleSummary.json(json);
    auto json_str = json.dump();
    for (int i = 0; i < json_str.length(); i += 150)
        logDebug("JSON [" << i << "]: " << json_str.substr(i, i + 150));
    cin.content = xsd::toAnyTypeUnnamed(json_str);

    request.req->primitiveContent = xsd::toAnyNamed(cin);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    if (response->responseStatusCode != xsd::m2m::ResponseStatusCode::CREATED)
    {
        logWarn("Content instance creation failed: " << toString(response->responseStatusCode));
        return false;
    }

    json_str = xsd::toAnyTypeUnnamed(response->primitiveContent).dumpJson();
    logInfo("Content instance creation: " << toString(response->responseStatusCode) << ", " << json_str);

    return true;
}

// Delete the content instance at the given path.  Not used in this demo.
bool delete_content_instance(const std::string& path)
{
    m2m::Request request = appEntity.newRequest(xsd::m2m::Operation::Delete, m2m::To{path});

    xsd::m2m::FilterCriteria fc;
    fc.filterUsage = xsd::m2m::FilterUsage::Discovery;
    fc.resourceType = xsd::m2m::ResourceTypeList();
    fc.resourceType->push_back(xsd::m2m::ResourceType::contentInstance);
    fc.level = 1;

    request.req->filterCriteria = std::move(fc);

    appEntity.sendRequest(request);
    auto response = appEntity.getResponse(request);

    if (response->responseStatusCode != xsd::m2m::ResponseStatusCode::DELETED
        && response->responseStatusCode != xsd::m2m::ResponseStatusCode::NOT_FOUND)
    {
        logWarn("Content instance deletion failed: " << toString(response->responseStatusCode));
        return false;
    }

    if (response->primitiveContent.empty())
    {
        logInfo("Content instance deletion: " << toString(response->responseStatusCode));
    }
    else
    {
        auto json = xsd::toAnyTypeUnnamed(response->primitiveContent).dumpJson();
        logInfo("Content instance deletion: " << toString(response->responseStatusCode) << ", " << json);
    }

    return true;
}

// WARNING Calling appEntity.sendRequest() from within this callback handler may deadlock the CoAP stack. Use a separate thread to
// send requests instead.
void notificationCallback(m2m::Notification notification)
{
    if (!notification.notificationEvent.isSet())
    {
        logWarn("Notification has no notificationEvent");
        return;
    }

    logDebug("Got notification type " << toString(notification.notificationEvent->notificationEventType));

    if (notification.notificationEvent->notificationEventType != xsd::m2m::NotificationEventType::Create_of_Direct_Child_Resource)
    {
        return;
    }

    auto contentInstance = notification.notificationEvent->representation->extractNamed<xsd::m2m::ContentInstance>();
    auto json_str = xsd::toAnyTypeUnnamed(contentInstance).dumpJson();
    for (int i = 0; i < json_str.length(); i += 150)
        logDebug("ContentInstance [" << i << "]: " << json_str.substr(i, i + 150));

    // Use the con element to decide how to handle the notification:
    //   * {"con":{"svcdat":...}...}: Accumulate the metersvc data
    //   * {"con":"{'reportInterval': 3600}",...}: Change our report interval (NOTE con is a JSON-like string in this case)
    try
    {
        auto json = nlohmann::json::parse(json_str);
        auto con = json.at("con");
        if (con.find("svcdat") != con.end())
        {
            auto meterRead = contentInstance.content->extractUnnamed<xsd::mtrsvc::MeterRead>();
            auto &meterSvcData = *meterRead.meterSvcData;
            parseMeterSvcData(meterSvcData);

            return;
        }

        if (con.is_string())
        {
            auto json = nlohmann::json::parse(singleQuoteToDoubleQuote(con.get<std::string>()));
            if (json.find("reportInterval") != json.end())
            {
                auto seconds = json.at("reportInterval");
                if (seconds.is_number_integer())
                    parseReportInterval(seconds);
                else
                    logWarn("Invalid report interval: " << con);
            }
            else
            {
                logInfo("Invalid string con: " << con.dump());
            }

            return;
        }

        logDebug("Notification has unknown content: " << con);
    }
    catch (const std::exception& e)
    {
        logError("Failed to parse content instance: " << e.what());
    }
}

void parseReportInterval(const int seconds)
{
    logInfo("Detected config \"" << seconds << "\"");
    if (seconds < 15 || seconds > 60 * 60 * 24 * 31)
    {
        logInfo("Report interval of " << seconds << " s out of bounds; not changing");
        return;
    }

    // Handle the interval change's impact on the next report time.  Note that if this results in a time in the past, a report will
    // be sent immediately the next time we receive metersvc data, and thereafter according to the new interval.
    reportTime += milliseconds((seconds - reportPeriod) * 1000);
    milliseconds timeNow = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());
    if (reportTime < timeNow)
        reportTime = timeNow;

    reportPeriod = seconds;
    logInfo("Report interval set to " << reportPeriod << " s");
}

void parseMeterSvcData(const xsd::mtrsvc::MeterSvcData& meterSvcData)
{
    // If the time to send a report has arrived, do so before parsing the new data.
    milliseconds timeNow = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());
    if (timeNow >= reportTime)
    {
        if (reportTime <= milliseconds(0))
        {
            // First run; set an initial report time and continue.
            reportTime = timeNow + milliseconds(reportPeriod * 1000 - 500);
            report.reset();
        }
        else
        {
            SampleSummary sampleSummary;
            report.summarise(sampleSummary);

            // NOTE Since we expect to be called from within the notification handler, we must call create_content_instance()
            // asynchronously, for which we use the sample queue.
            reportQueue.push(sampleSummary);
            logDebug("Queued summary of " << sampleSummary.count << " samples");

            report.reset();
            reportTime += milliseconds(reportPeriod * 1000);
            if (reportTime <= timeNow)                          // Sanity check
            {
                logWarn("Report time in the past; resetting to " << reportPeriod << " s from now");
                reportTime = timeNow + milliseconds(reportPeriod * 1000);
            }
        }
    }

    logInfo("timestamp: " << meterSvcData.readTimeLocal);
    Sample sample;
    if (SPOOF_METER)
    {
        spoofSample(sample);
        report.accumulate(sample);
        logInfo("Accumulated " << report.count() << (report.count() == 1 ? " sample" : " samples"));
    }
    else if (meterSvcData.powerQuality.isSet())
    {
        logDebug("powerQuality: " << *meterSvcData.powerQuality);
        sample.set(meterSvcData.powerQuality.getValue());
        report.accumulate(sample);
        logInfo("Accumulated " << report.count() << (report.count() == 1 ? " sample" : " samples"));
    }
    else if (meterSvcData.summations.isSet())
    {
        // Summation data are not used in this demo.
        logDebug("summations: " << *meterSvcData.summations);
    }
}
