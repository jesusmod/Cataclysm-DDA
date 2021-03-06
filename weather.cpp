#include <vector>
#include <sstream>
#include "item_factory.h"
#include "options.h"
#include "game.h"
#include "weather.h"

#define PLAYER_OUTSIDE (g->m.is_outside(g->u.posx, g->u.posy) && g->levz >= 0)
#define THUNDER_CHANCE 50
#define LIGHTNING_CHANCE 600

void weather_effect::glare(game *g)
{
 if (PLAYER_OUTSIDE && g->is_in_sunlight(g->u.posx, g->u.posy) && !g->u.is_wearing("sunglasses") &&
     !g->u.has_bionic("bio_sunglasses"))
  g->u.infect("glare", bp_eyes, 1, 2, g);
}

void fill_funnels(game *g, int rain_depth_mm_per_hour, bool acid, trap_id t)
{
    int funnel_radius_mm = 0;
    switch (t) {
        case tr_funnel:             funnel_radius_mm = 380; break;
        case tr_makeshift_funnel:   funnel_radius_mm =  85; break;
        default: return;
    }

    // How many turns should it take for us to collect 1 charge of rainwater?
    item water(item_controller->find_template("water"), 0);
    const double charge_ml = (double) (water.weight()) / water.charges;
    const double PI = 3.14159265358979f;
    const double surface_area_mm2 = PI * (funnel_radius_mm * funnel_radius_mm);
    const double vol_mm3_per_hour = surface_area_mm2 * rain_depth_mm_per_hour;
    const double vol_mm3_per_turn = vol_mm3_per_hour / 600;
    const double ml_to_mm3 = 1000;
    const double turns_per_charge = charge_ml * ml_to_mm3 / vol_mm3_per_turn;

    item *c = NULL;
    char maxcontains = 0;

    // Give each funnel on the map a chance to collect the rain.
    std::set<point> funnel_locs = g->m.trap_locations(t);
    std::set<point>::iterator i;
    for (i = funnel_locs.begin(); i != funnel_locs.end(); ++i) {
        point loc = *i;
        std::vector<item>& items = g->m.i_at(loc.x, loc.y);
        if (one_in(turns_per_charge)) {
            // This funnel has collected some rain! Put the rain in the largest
            // container here which is either empty or contains some mixture of
            // impure water and acid.
            for (int j = 0; j < items.size(); j++) {
                item *it = &(items[j]);
                if (it->is_container() && it->has_flag("WATERTIGHT") && it->has_flag("SEALS")) {
                    it_container* ct = dynamic_cast<it_container*>(it->type);
                    if (ct->contains > maxcontains && (
                            it->contents.empty() ||
                            it->contents[0].typeId() == "water" ||
                            it->contents[0].typeId() == "water_acid" ||
                            it->contents[0].typeId() == "water_acid_weak")) {
                        c = it;
                        maxcontains = ct->contains;
                    }
                }
            }
        }
    }

    // If we found an eligible container, add some rain to it (or consider
    // contaminating its current liquid).
    if (c != NULL) {
        const char *typeId = acid ? "water_acid" : "water";
        if (c->contents.empty()) {
            // This is easy. Just add 1 charge of the rain liquid to the container.
            item ret(item_controller->find_template(typeId), 0);
            if (!acid) {
                // Funnels aren't always clean enough for water.
                ret.poison = one_in(10) ? 1 : 0;
            }
            c->put_in(ret);
        } else {
            // The container already has a liquid.
            item &liq = c->contents[0];

            it_container* ct = dynamic_cast<it_container*>(c->type);
            if (liq.charges < ct->contains) {
                liq.charges++;
            }

            if (liq.typeId() == typeId || liq.typeId() == "water_acid_weak") {
                // The container already contains this liquid or weakly acidic water.
                // Don't do anything special -- we already added liquid.
            } else {
                // The rain is different from what's in the container.
                // Turn the container's liquid into weak acid with a probability
                // based on its current volume.

                // If it's raining acid and this container started with 7
                // charges of water, the liquid will now be 1/8th acid or,
                // equivalently, 1/4th weak acid (the rest being water). A
                // stochastic approach gives the liquid a 1 in 4 (or 2 in
                // liquid.charges) chance of becoming weak acid.
                const bool transmute = x_in_y(2, liq.charges);

                if (transmute) {
                    item transmuted(item_controller->find_template("water_acid_weak"), 0);
                    transmuted.charges = liq.charges;
                    c->contents[0] = transmuted;
                } else if (liq.typeId() == "water") {
                    // The container has water, and the acid rain didn't turn it
                    // into weak acid. Poison the water instead, assuming 1
                    // charge of acid would act like a charge of water with poison 5.
                    int total_poison = liq.poison * (liq.charges - 1) + 5;
                    liq.poison = total_poison / liq.charges;
                    int leftover_poison = total_poison - liq.poison * liq.charges;
                    if (leftover_poison > rng(0, liq.charges)) {
                        liq.poison++;
                    }
                }
            }
        }
    }
}

void fill_water_collectors(game *g, int mmPerHour, bool acid)
{
    fill_funnels(g, mmPerHour, acid, tr_funnel);
    fill_funnels(g, mmPerHour, acid, tr_makeshift_funnel);
}

void decay_fire_and_scent(game *g, int fire_amount)
{
    for (int x = g->u.posx - SEEX * 2; x <= g->u.posx + SEEX * 2; x++) {
        for (int y = g->u.posy - SEEY * 2; y <= g->u.posy + SEEY * 2; y++) {
            if (g->m.is_outside(x, y)) {
                field_entry *fd = (g->m.field_at(x, y).findField(fd_fire));
                if (fd) {
                    fd->setFieldAge(fd->getFieldAge() + fire_amount);
                }
                if (g->scent(x, y) > 0)
                g->scent(x, y)--;
            }
        }
    }
}

void generic_wet(game *g, bool acid)
{
    if ((!g->u.is_wearing("coat_rain") || one_in(50)) &&
         (!g->u.weapon.has_flag("RAIN_PROTECT") || one_in(10)) && !g->u.has_trait("FEATHERS") &&
         (g->u.warmth(bp_torso) * 4/5 + g->u.warmth(bp_head) / 5) < 30 && PLAYER_OUTSIDE &&
         one_in(2)) {
        g->u.drench(g, 30 - (g->u.warmth(bp_torso) * 4/5 + g->u.warmth(bp_head) / 5),
                     mfb(bp_torso)|mfb(bp_arms)|mfb(bp_head));
    }

    fill_water_collectors(g, 4, acid);
    decay_fire_and_scent(g, 15);
}

void generic_very_wet(game *g, bool acid)
{
    if ((!g->u.is_wearing("coat_rain") || one_in(25)) &&
         (!g->u.weapon.has_flag("RAIN_PROTECT") || one_in(5)) && !g->u.has_trait("FEATHERS") &&
         (g->u.warmth(bp_torso) * 4/5 + g->u.warmth(bp_head) / 5) < 60 && PLAYER_OUTSIDE) {
        g->u.drench(g, 60 - (g->u.warmth(bp_torso) * 4/5 + g->u.warmth(bp_head) / 5),
                     mfb(bp_torso)|mfb(bp_arms)|mfb(bp_head));
    }

    fill_water_collectors(g, 8, acid);
    decay_fire_and_scent(g, 45);
}

void weather_effect::wet(game *g)
{
    generic_wet(g, false);
}

void weather_effect::very_wet(game *g)
{
    generic_very_wet(g, false);
}

void weather_effect::thunder(game *g)
{
    very_wet(g);
    if (one_in(THUNDER_CHANCE)) {
        if (g->levz >= 0) {
            g->add_msg(_("You hear a distant rumble of thunder."));
        } else if (!g->u.has_trait("BADHEARING") && one_in(1 - 3 * g->levz)) {
            g->add_msg(_("You hear a rumble of thunder from above."));
        }
    }
}

void weather_effect::lightning(game *g)
{
    thunder(g);
}

void weather_effect::light_acid(game *g)
{
    generic_wet(g, true);
    if (int(g->turn) % 10 == 0 && PLAYER_OUTSIDE) {
        if (g->u.weapon.has_flag("RAIN_PROTECT") && one_in(2)) {
            g->add_msg(_("Your %s protects you from the acidic drizzle."), g->u.weapon.name.c_str());
        } else {
            if (g->u.is_wearing("coat_rain") && !one_in(3)) {
                g->add_msg(_("Your raincoat protects you from the acidic drizzle."));
            } else {
                bool has_helmet = false;
                if (g->u.is_wearing_power_armor(&has_helmet) && (has_helmet || !one_in(4))) {
                    g->add_msg(_("Your power armor protects you from the acidic drizzle."));
                } else {
                    g->add_msg(_("The acid rain stings, but is mostly harmless for now..."));
                    if (one_in(10) && (g->u.pain < 10)) {g->u.pain++;}
                }
            }
        }
    }
}

void weather_effect::acid(game *g)
{
    if (int(g->turn) % 2 == 0 && PLAYER_OUTSIDE) {
        if (g->u.weapon.has_flag("RAIN_PROTECT") && one_in(4)) {
            g->add_msg(_("Your umbrella protects you from the acid rain."));
        } else {
            if (g->u.is_wearing("coat_rain") && one_in(2)) {
                g->add_msg(_("Your raincoat protects you from the acid rain."));
            } else {
                bool has_helmet = false;
                if (g->u.is_wearing_power_armor(&has_helmet) && (has_helmet || !one_in(2))) {
                    g->add_msg(_("Your power armor protects you from the acidic drizzle."));
                } else {
                    g->add_msg(_("The acid rain burns!"));
                    if (one_in(2) && (g->u.pain < 100)) {g->u.pain += rng(1, 5);}
                }
            }
        }
    }

    if (g->levz >= 0) {
        for (int x = g->u.posx - SEEX * 2; x <= g->u.posx + SEEX * 2; x++) {
            for (int y = g->u.posy - SEEY * 2; y <= g->u.posy + SEEY * 2; y++) {
                if (!g->m.has_flag(diggable, x, y) && !g->m.has_flag(noitem, x, y) &&
                      g->m.move_cost(x, y) > 0 && g->m.is_outside(x, y) && one_in(400)) {
                    g->m.add_field(g, x, y, fd_acid, 1);
                }
            }
        }
    }
    for (int i = 0; i < g->num_zombies(); i++) {
        if (g->m.is_outside(g->zombie(i).posx(), g->zombie(i).posy())) {
            if (!g->zombie(i).has_flag(MF_ACIDPROOF)) {
                g->zombie(i).hurt(1);
            }
        }
    }
    generic_very_wet(g, true);
}


// Script from wikipedia:
// Current time
// The current time is hour/minute Eastern Standard Time
// Local conditions
// At 8 AM in Falls City, it was sunny. The temperature was 60 degrees, the dewpoint 59,
// and the relative humidity 97%. The wind was west at 6 miles (9.7 km) an hour.
// The pressure was 30.00 inches (762 mm) and steady.
// Regional conditions
// Across eastern Nebraska, southwest Iowa, and northwest Missouri, skies ranged from
// sunny to mostly sunny. It was 60 at Beatrice, 59 at Lincoln, 59 at Nebraska City, 57 at Omaha,
// 59 at Red Oak, and 62 at St. Joseph."
// Forecast
// TODAY...MOSTLY SUNNY. HIGHS IN THE LOWER 60S. NORTHEAST WINDS 5 TO 10 MPH WITH GUSTS UP TO 25 MPH.
// TONIGHT...MOSTLY CLEAR. LOWS IN THE UPPER 30S. NORTHEAST WINDS 5 TO 10 MPH.
// MONDAY...MOSTLY SUNNY. HIGHS IN THE LOWER 60S. NORTHEAST WINDS 10 TO 15 MPH.
// MONDAY NIGHT...PARTLY CLOUDY. LOWS AROUND 40. NORTHEAST WINDS 5 TO 10 MPH.

// 0% – No mention of precipitation
// 10% – No mention of precipitation, or isolated/slight chance
// 20% – Isolated/slight chance
// 30% – (Widely) scattered/chance
// 40% or 50% – Scattered/chance
// 60% or 70% – Numerous/likely
// 80%, 90% or 100% – No additional modifiers (i.e. "showers and thunderstorms")

std::string weather_forecast(game *g, radio_tower tower)
{
    std::stringstream weather_report;
    // Local conditions
    city *closest_city = &g->cur_om->cities[g->cur_om->closest_city(point(tower.x, tower.y))];
    // Current time
    weather_report << string_format(
        _("The current time is %s Eastern Standard Time.  At %s in %s, it was %s. The temperature was %s"),
        g->turn.print_time().c_str(), g->turn.print_time(true).c_str(), closest_city->name.c_str(),
        weather_data[g->weather].name.c_str(), print_temperature(g->temperature).c_str()
    );

    //weather_report << ", the dewpoint ???, and the relative humidity ???.  ";
    //weather_report << "The wind was <direction> at ? mi/km an hour.  ";
    //weather_report << "The pressure was ??? in/mm and steady/rising/falling.";

    // Regional conditions (simulated by chosing a random range containing the current conditions).
    // Adjusted for weather volatility based on how many weather changes are coming up.
    //weather_report << "Across <region>, skies ranged from <cloudiest> to <clearest>.  ";
    // TODO: Add fake reports for nearby cities

    // TODO: weather forecast
    // forecasting periods are divided into 12-hour periods, day (6-18) and night (19-5)
    // Accumulate percentages for each period of various weather statistics, and report that
    // (with fuzz) as the weather chances.
    int weather_proportions[NUM_WEATHER_TYPES] = {0};
    signed char high = 0;
    signed char low = 0;
    calendar start_time = g->turn;
    int period_start = g->turn.hours();
    // TODO wind direction and speed
    for( std::list<weather_segment>::iterator period = g->future_weather.begin();
         period != g->future_weather.end(); ++period )
    {
        int period_deadline = period->deadline.hours();
        signed char period_temperature = period->temperature;
        weather_type period_weather = period->weather;
        bool start_day = period_start >= 6 && period_start <= 18;
        bool end_day = period_deadline >= 6 && period_deadline <= 18;

        high = std::max(high, period_temperature);
        low = std::min(low, period_temperature);

        if(start_day != end_day) // Assume a period doesn't last over 12 hrs?
        {
            weather_proportions[period_weather] += end_day ? 6 : 18 - period_start;
            int weather_duration = 0;
            int predominant_weather = 0;
            std::string day;
            if( g->turn.days() == period->deadline.days() )
            {
                if( start_day )
                {
                    day = _("Today");
                }
                else
                {
                    day = _("Tonight");
                }
            }
            else
            {
                std::string dayofweak = start_time.day_of_week();
                if( !start_day )
                {
                    day = rmp_format(_("<Mon Night>%s Night"), dayofweak.c_str());
                }
                else
                {
                    day = dayofweak;
                }
            }
            for( int i = WEATHER_CLEAR; i < NUM_WEATHER_TYPES; i++)
            {
                if( weather_proportions[i] > weather_duration)
                {
                    weather_duration = weather_proportions[i];
                    predominant_weather = i;
                }
            }
            // Print forecast
            weather_report << string_format(
                _("%s...%s. Highs of %s. Lows of %s. "),
                day.c_str(), weather_data[predominant_weather].name.c_str(),
                print_temperature(high).c_str(), print_temperature(low).c_str()
            );
            low = period_temperature;
            high = period_temperature;
            weather_proportions[period_weather] += end_day ? 6 : 18 - period_start;
        } else {
            weather_proportions[period_weather] += period_deadline - period_start;
        }
        start_time = period->deadline;
        period_start = period_deadline;
    }
    return weather_report.str();
}

std::string print_temperature(float fahrenheit, int decimals)
{
    std::stringstream ret;
    ret.precision(decimals);
    ret << std::fixed;

    if(OPTIONS["USE_CELSIUS"] == "celsius")
    {
        ret << ((fahrenheit-32) * 5 / 9);
        return rmp_format(_("<Celsius>%sC"), ret.str().c_str());
    }
    else
    {
        ret << fahrenheit;
        return rmp_format(_("<Fahrenheit>%sF"), ret.str().c_str());
    }

}
