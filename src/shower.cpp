/*
 * EMS-ESP - https://github.com/emsesp/EMS-ESP
 * Copyright 2020-2023  Paul Derbyshire
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shower.h"

namespace emsesp {

uuid::log::Logger Shower::logger_{F_(shower), uuid::log::Facility::CONSOLE};

static bool force_coldshot = false;

void Shower::start() {
    EMSESP::webSettingsService.read([&](WebSettings & settings) {
        shower_timer_          = settings.shower_timer;
        shower_alert_          = settings.shower_alert;
        shower_alert_trigger_  = settings.shower_alert_trigger * 60000; // convert from minutes
        shower_alert_coldshot_ = settings.shower_alert_coldshot * 1000; // convert from seconds
    });

    Command::add(
        EMSdevice::DeviceType::BOILER,
        F_(coldshot),
        [&](const char * value, const int8_t id, JsonObject & output) {
            LOG_INFO("Forcing coldshot...");
            if (shower_state_) {
                output["message"] = "OK";
                force_coldshot    = true;
            } else {
                output["message"] = "Coldshot failed. Shower not active";
                LOG_WARNING("Coldshot failed. Shower not active");
                force_coldshot = false;
            }
            return true;
        },
        FL_(coldshot_cmd),
        CommandFlag::ADMIN_ONLY);

    if (shower_timer_) {
        set_shower_state(false, true); // turns shower to off and creates HA topic if not already done
    }
}

void Shower::loop() {
    if (!shower_timer_) {
        return;
    }

    uint32_t time_now = uuid::get_uptime();

    // if already in cold mode, ignore all this logic until we're out of the cold blast
    if (!doing_cold_shot_) {
        // is the hot water running?
        if (EMSESP::tap_water_active()) {
            // if heater was previously off, start the timer
            if (timer_start_ == 0) {
                // hot water just started...
                timer_start_     = time_now;
                timer_pause_     = 0; // remove any last pauses
                doing_cold_shot_ = false;
                duration_        = 0;
                shower_state_    = false;
            } else {
                // hot water has been  on for a while
                // first check to see if hot water has been on long enough to be recognized as a Shower/Bath
                if (!shower_state_ && (time_now - timer_start_) > SHOWER_MIN_DURATION) {
                    set_shower_state(true);
                    LOG_DEBUG("hot water still running, starting shower timer");
                }
                // check if the shower has been on too long
                else if ((shower_alert_ && ((time_now - timer_start_) > shower_alert_trigger_)) || force_coldshot) {
                    shower_alert_start();
                }
            }
        } else { // hot water is off
            // if it just turned off, record the time as it could be a short pause
            if (timer_start_ && (timer_pause_ == 0)) {
                timer_pause_ = time_now;
            }

            // if shower has been off for longer than the wait time
            if (timer_pause_ && ((time_now - timer_pause_) > SHOWER_PAUSE_TIME)) {
                // it is over the wait period, so assume that the shower has finished and calculate the total time and publish
                // because its unsigned long, can't have negative so check if length is less than OFFSET_TIME
                if ((timer_pause_ - timer_start_) > SHOWER_OFFSET_TIME) {
                    duration_ = (timer_pause_ - timer_start_ - SHOWER_OFFSET_TIME);
                    if (duration_ > SHOWER_MIN_DURATION) {
                        StaticJsonDocument<EMSESP_JSON_SIZE_SMALL> doc;

                        // duration in seconds
                        doc["duration"] = (duration_ / 1000UL); // seconds
                        time_t now      = time(nullptr);
                        // if NTP enabled, publish timestamp
                        if (now > 1576800000) { // year 2020
                            // doc["timestamp_s"] = now; // if needed, in seconds
                            tm * tm_ = localtime(&now);
                            char dt[25];
                            strftime(dt, sizeof(dt), "%FT%T%z", tm_);
                            doc["timestamp"] = dt;
                            LOG_INFO("shower finished (duration %s)", dt);
                        } else {
                            LOG_INFO("shower finished (duration %lu s)", duration_ / 1000UL);
                        }
                        Mqtt::queue_publish("shower_data", doc.as<JsonObject>());
                    }
                }

                // reset everything
                timer_start_       = 0;
                timer_pause_       = 0;
                doing_cold_shot_   = false;
                alert_timer_start_ = 0;

                set_shower_state(false);
            }
        }
        return;
    }

    // at this point we're in the shower cold shot (doing_cold_shot_ == true)
    // keep repeating until the time is up
    if ((time_now - alert_timer_start_) > shower_alert_coldshot_) {
        shower_alert_stop();
    }
}

// turn off hot water to send a shot of cold
void Shower::shower_alert_start() {
    LOG_DEBUG("Shower Alert started");
    (void)Command::call(EMSdevice::DeviceType::BOILER, "wwtapactivated", "false");
    doing_cold_shot_   = true;
    force_coldshot     = false;
    alert_timer_start_ = uuid::get_uptime(); // timer starts now
}

// turn back on the hot water for the shower
void Shower::shower_alert_stop() {
    if (doing_cold_shot_) {
        LOG_DEBUG("Shower Alert stopped");
        (void)Command::call(EMSdevice::DeviceType::BOILER, "wwtapactivated", "true");
        doing_cold_shot_ = false;
        force_coldshot   = false;
    }
}

// send status of shower to MQTT topic called shower_active - which is determined by the state parameter
// and creates the HA config topic if HA enabled
// force is used by EMSESP::publish_all_loop()
void Shower::set_shower_state(bool state, bool force) {
    // sets the state
    shower_state_ = state;

    // only publish if that state has changed
    static bool old_shower_state_ = false;
    if ((shower_state_ == old_shower_state_) && !force) {
        return;
    }
    old_shower_state_ = shower_state_; // copy current state

    // always publish as a string
    char s[12];
    Mqtt::queue_publish("shower_active", Helpers::render_boolean(s, shower_state_)); // https://github.com/emsesp/EMS-ESP/issues/369

    // send out HA MQTT Discovery config topic
    if ((Mqtt::ha_enabled()) && (!ha_configdone_ || force)) {
        StaticJsonDocument<EMSESP_JSON_SIZE_LARGE> doc;
        char                                       topic[Mqtt::MQTT_TOPIC_MAX_SIZE];
        char                                       str[70];
        char                                       stat_t[50];

        //
        // shower active
        //
        doc["name"] = "Shower Active";

        if (Mqtt::entity_format() == Mqtt::entityFormat::MULTI_SHORT) {
            snprintf(str, sizeof(str), "%s_shower_active", Mqtt::basename().c_str());
        } else {
            snprintf(str, sizeof(str), "shower_active"); // v3.4 compatible
        }
        doc["uniq_id"]   = str;
        doc["object_id"] = str;

        snprintf(stat_t, sizeof(stat_t), "%s/shower_active", Mqtt::base().c_str());
        doc["stat_t"] = stat_t;

        if (EMSESP::system_.bool_format() == BOOL_FORMAT_TRUEFALSE) {
            doc["pl_on"]  = "true";
            doc["pl_off"] = "false";
        } else if (EMSESP::system_.bool_format() == BOOL_FORMAT_10) {
            doc["pl_on"]  = 1;
            doc["pl_off"] = 0;
        } else {
            char result[12];
            doc["pl_on"]  = Helpers::render_boolean(result, true);
            doc["pl_off"] = Helpers::render_boolean(result, false);
        }

        JsonObject dev = doc.createNestedObject("dev");
        dev["name"]    = "EMS-ESP Shower";
        JsonArray ids  = dev.createNestedArray("ids");
        ids.add(Mqtt::basename() + "-shower");

        Mqtt::add_avty_to_doc(stat_t, doc.as<JsonObject>()); // add "availability" section

        snprintf(topic, sizeof(topic), "binary_sensor/%s/shower_active/config", Mqtt::basename().c_str());
        ha_configdone_ = Mqtt::queue_ha(topic, doc.as<JsonObject>()); // publish the config payload with retain flag

        //
        // shower duration
        //
        doc.clear();

        snprintf(str, sizeof(str), "%s_shower_duration", Mqtt::basename().c_str());

        doc["uniq_id"]   = str;
        doc["object_id"] = str;

        snprintf(stat_t, sizeof(stat_t), "%s/shower_data", Mqtt::base().c_str());
        doc["stat_t"] = stat_t;

        doc["name"]         = "Shower Duration";
        doc["val_tpl"]      = "{{value_json.duration if value_json.duration is defined else 0}}";
        doc["unit_of_meas"] = "s";
        doc["stat_cla"]     = "measurement";
        doc["dev_cla"]      = "duration";
        // doc["ent_cat"]      = "diagnostic";

        JsonObject dev2 = doc.createNestedObject("dev");
        dev2["name"]    = "EMS-ESP Shower";
        JsonArray ids2  = dev2.createNestedArray("ids");
        ids2.add(Mqtt::basename() + "-shower");

        Mqtt::add_avty_to_doc(stat_t, doc.as<JsonObject>(), "value_json.duration is defined"); // add "availability" section

        snprintf(topic, sizeof(topic), "sensor/%s/shower_duration/config", Mqtt::basename().c_str());
        Mqtt::queue_ha(topic, doc.as<JsonObject>()); // publish the config payload with retain flag

        //
        // shower timestamp
        //
        doc.clear();

        snprintf(str, sizeof(str), "%s_shower_timestamp", Mqtt::basename().c_str());

        doc["uniq_id"]   = str;
        doc["object_id"] = str;

        snprintf(stat_t, sizeof(stat_t), "%s/shower_data", Mqtt::base().c_str());
        doc["stat_t"] = stat_t;

        doc["name"]    = "Shower Timestamp";
        doc["val_tpl"] = "{{value_json.timestamp if value_json.timestamp is defined else 0}}";
        // doc["ent_cat"] = "diagnostic";

        JsonObject dev3 = doc.createNestedObject("dev");
        dev3["name"]    = "EMS-ESP Shower";
        JsonArray ids3  = dev3.createNestedArray("ids");
        ids3.add(Mqtt::basename() + "-shower");

        Mqtt::add_avty_to_doc(stat_t, doc.as<JsonObject>(), "value_json.timestamp is defined"); // add "availability" section

        snprintf(topic, sizeof(topic), "sensor/%s/shower_timestamp/config", Mqtt::basename().c_str());
        Mqtt::queue_ha(topic, doc.as<JsonObject>()); // publish the config payload with retain flag
    }
}

} // namespace emsesp
