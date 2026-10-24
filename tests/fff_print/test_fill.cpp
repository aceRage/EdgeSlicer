#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Fill/FillGyroid.hpp"
#include "libslic3r/Fill/FillTpmsAdaptive.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r;

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle = 0, double density = 1.0);

#if 0
TEST_CASE("Fill: adjusted solid distance") {
    int surface_width = 250;
    int distance = Slic3r::Flow::solid_spacing(surface_width, 47);
    REQUIRE(distance == Approx(50));
    REQUIRE(surface_width % distance == 0);
}
#endif

TEST_CASE("Fill: Pattern Path Length", "[Fill]") {
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
    filler->angle = float(-(PI)/2.0);
	FillParams fill_params;
	filler->spacing = 5;
	fill_params.dont_adjust = true;
	//fill_params.endpoints_overlap = false;
	fill_params.density = float(filler->spacing / 50.0);

    auto test = [&filler, &fill_params] (const ExPolygon& poly) -> Slic3r::Polylines {
        Slic3r::Surface surface(stTop, poly);
        return filler->fill_surface(&surface, fill_params);
    };

    SECTION("Square") {
        Slic3r::Points test_set;
        test_set.reserve(4);
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        for (size_t i = 0; i < 4; ++i) {
            std::transform(points.cbegin()+i, points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } ); 
            std::transform(points.cbegin(), points.cbegin()+i, std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
            Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
            REQUIRE(paths.size() == 1); // one continuous path

            // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
            // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
            // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
            REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length

            test_set.clear();
        }
    }
    SECTION("Diamond with endpoints on grid") {
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(150,50), Vec2d(100,100), Vec2d(0,100), Vec2d(-50,50)};
        Slic3r::Points test_set;
        test_set.reserve(6);
        std::transform(points.cbegin(), points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
        REQUIRE(paths.size() == 1); // one continuous path
    }

    SECTION("Square with hole") {
        std::vector<Vec2d> square {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        std::vector<Vec2d> hole {Vec2d(25,25), Vec2d(75,25), Vec2d(75,75), Vec2d(25,75) };
        std::reverse(hole.begin(), hole.end());

        Slic3r::Points test_hole;
        Slic3r::Points test_square;

        std::transform(square.cbegin(), square.cend(), std::back_inserter(test_square), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        std::transform(hole.cbegin(), hole.cend(), std::back_inserter(test_hole), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );

        for (double angle : {-(PI/2.0), -(PI/4.0), -(PI), PI/2.0, PI}) {
            for (double spacing : {25.0, 5.0, 7.5, 8.5}) {
				fill_params.density = float(filler->spacing / spacing);
                filler->angle = float(angle);
                ExPolygon e(test_square, test_hole);
                Slic3r::Polylines paths = test(e);
#if 0
				{
					BoundingBox bbox = get_extents(e);
					SVG svg("c:\\data\\temp\\square_with_holes.svg", bbox);
					svg.draw(e);
					svg.draw(paths);
					svg.Close();
				}
#endif
                REQUIRE((paths.size() >= 1 && paths.size() <= 3));
                // paths don't cross hole
                REQUIRE(diff_pl(paths, offset(e, float(SCALED_EPSILON*10))).size() == 0);
            }
        }
    }
    SECTION("Regression: Missing infill segments in some rare circumstances") {
        filler->angle = float(PI/4.0);
		fill_params.dont_adjust = false;
        filler->spacing = 0.654498;
        //filler->endpoints_overlap = unscale(359974);
		fill_params.density = 1;
        filler->layer_id = 66;
        filler->z = 20.15;

        Slic3r::Points points {Point(25771516,14142125),Point(14142138,25771515),Point(2512749,14142131),Point(14142125,2512749)};
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(points));
        REQUIRE(paths.size() == 1); // one continuous path

        // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
        // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
        // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
        REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length
    }

    SECTION("Rotated Square") {
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(50,0), Point::new_scale(50,50), Point::new_scale(0,50)};
        Slic3r::ExPolygon expolygon(square);
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
		filler->bounding_box = get_extents(expolygon.contour);
        filler->angle = 0;
        
        Surface surface(stTop, expolygon);
        auto flow = Slic3r::Flow(0.69f, 0.4f, 0.50f);

		FillParams fill_params;
		fill_params.density = 1.0;
		filler->spacing = flow.spacing();

        for (auto angle : { 0.0, 45.0}) {
            surface.expolygon.rotate(angle, Point(0,0));
            Polylines paths = filler->fill_surface(&surface, fill_params);
            REQUIRE(paths.size() == 1);
        }
    }

    #if 0   // Disabled temporarily due to precission issues on the Mac VM
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(6883102, 9598327.01296997),
            Point::new_scale(6883102, 20327272.01297),
            Point::new_scale(3116896, 20327272.01297),
            Point::new_scale(3116896, 9598327.01296997) 
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        for (size_t i = 0; i <= 20; ++i)
        {
            expolygon.scale(1.05);
            REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        }
    }
    #endif

    SECTION("Solid surface fill") {
        Slic3r::Points points {
                Slic3r::Point(59515297,5422499),Slic3r::Point(59531249,5578697),Slic3r::Point(59695801,6123186),
                Slic3r::Point(59965713,6630228),Slic3r::Point(60328214,7070685),Slic3r::Point(60773285,7434379),
                Slic3r::Point(61274561,7702115),Slic3r::Point(61819378,7866770),Slic3r::Point(62390306,7924789),
                Slic3r::Point(62958700,7866744),Slic3r::Point(63503012,7702244),Slic3r::Point(64007365,7434357),
                Slic3r::Point(64449960,7070398),Slic3r::Point(64809327,6634999),Slic3r::Point(65082143,6123325),
                Slic3r::Point(65245005,5584454),Slic3r::Point(65266967,5422499),Slic3r::Point(66267307,5422499),
                Slic3r::Point(66269190,8310081),Slic3r::Point(66275379,17810072),Slic3r::Point(66277259,20697500),
                Slic3r::Point(65267237,20697500),Slic3r::Point(65245004,20533538),Slic3r::Point(65082082,19994444),
                Slic3r::Point(64811462,19488579),Slic3r::Point(64450624,19048208),Slic3r::Point(64012101,18686514),
                Slic3r::Point(63503122,18415781),Slic3r::Point(62959151,18251378),Slic3r::Point(62453416,18198442),
                Slic3r::Point(62390147,18197355),Slic3r::Point(62200087,18200576),Slic3r::Point(61813519,18252990),
                Slic3r::Point(61274433,18415918),Slic3r::Point(60768598,18686517),Slic3r::Point(60327567,19047892),
                Slic3r::Point(59963609,19493297),Slic3r::Point(59695865,19994587),Slic3r::Point(59531222,20539379),
                Slic3r::Point(59515153,20697500),Slic3r::Point(58502480,20697500),Slic3r::Point(58502480,5422499)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55, PI/2.0) == true);
    }
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(0,0),Point::new_scale(98,0),Point::new_scale(98,10), Point::new_scale(0,10)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.5, 45.0, 0.99) == true);
    }
}

/*
{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([4,0], [10,0], [15,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([15,0], [10,0], [4,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

for my $pattern (qw(rectilinear honeycomb hilbertcurve concentric)) {
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('fill_pattern', $pattern);
    $config->set('external_fill_pattern', $pattern);
    $config->set('perimeters', 1);
    $config->set('skirts', 0);
    $config->set('fill_density', 20);
    $config->set('layer_height', 0.05);
    $config->set('perimeter_extruder', 1);
    $config->set('infill_extruder', 2);
    my $print = Slic3r::Test::init_print('20mm_cube', config => $config, scale => 2);
    ok my $gcode = Slic3r::Test::gcode($print), "successful $pattern infill generation";
    my $tool = undef;
    my @perimeter_points = my @infill_points = ();
    Slic3r::GCode::Reader->new->parse($gcode, sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->perimeter_extruder-1) {
            push @perimeter_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            } elsif ($tool == $config->infill_extruder-1) {
            push @infill_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            }
            }
            });
    my $convex_hull = convex_hull(\@perimeter_points);
    ok !(defined first { !$convex_hull->contains_point($_) } @infill_points), "infill does not exceed perimeters ($pattern)";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('infill_only_where_needed', 1);
    $config->set('bottom_solid_layers', 0);
    $config->set('infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.5);
    $config->set('fill_density', 40);
    $config->set('cooling', 0);                 # for preventing speeds from being altered
        $config->set('first_layer_speed', '100%');  # for preventing speeds from being altered

        my $test = sub {
            my $print = Slic3r::Test::init_print('pyramid', config => $config);

            my $tool = undef;
            my @infill_extrusions = ();  # array of polylines
                Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
                        my ($self, $cmd, $args, $info) = @_;

                        if ($cmd =~ /^T(\d+)/) {
                        $tool = $1;
                        } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
                        if ($tool == $config->infill_extruder-1) {
                        push @infill_extrusions, Slic3r::Line->new_scale(
                                [ $self->X, $self->Y ],
                                [ $info->{new_X}, $info->{new_Y} ],
                                );
                        }
                        }
                        });
            return 0 if !@infill_extrusions;  # prevent calling convex_hull() with no points

                my $convex_hull = convex_hull([ map $_->pp, map @$_, @infill_extrusions ]);
            return unscale unscale sum(map $_->area, @{offset([$convex_hull], scale(+$config->infill_extrusion_width/2))});
        };

    my $tolerance = 5;  # mm^2

        $config->set('solid_infill_below_area', 0);
    ok $test->() < $tolerance,
       'no infill is generated when using infill_only_where_needed on a pyramid';

    $config->set('solid_infill_below_area', 70);
    ok abs($test->() - $config->solid_infill_below_area) < $tolerance,
       'infill is only generated under the forced solid shells';
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 1);
    $config->set('fill_density', 0);
    $config->set('top_solid_layers', 0);
    $config->set('bottom_solid_layers', 0);
    $config->set('solid_infill_below_area', 20000000);
    $config->set('solid_infill_every_layers', 2);
    $config->set('perimeter_speed', 99);
    $config->set('external_perimeter_speed', 99);
    $config->set('cooling', 0);
    $config->set('first_layer_speed', '100%');

    my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %layers_with_extrusion = ();
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd eq 'G1' && $info->{dist_XY} > 0 && $info->{extruding}) {
            if (($args->{F} // $self->F) != $config->perimeter_speed*60) {
            $layers_with_extrusion{$self->Z} = ($args->{F} // $self->F);
            }
            }
            });

    ok !%layers_with_extrusion,
       "solid_infill_below_area and solid_infill_every_layers are ignored when fill_density is 0";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 3);
    $config->set('fill_density', 0);
    $config->set('layer_height', 0.2);
    $config->set('first_layer_height', 0.2);
    $config->set('nozzle_diameter', [0.35]);
    $config->set('infill_extruder', 2);
    $config->set('solid_infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.52);
    $config->set('solid_infill_extrusion_width', 0.52);
    $config->set('first_layer_extrusion_width', 0);

    my $print = Slic3r::Test::init_print('A', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->infill_extruder-1) {
            my $z = 1 * $self->Z;
            $infill{$z} ||= [];
            push @{$infill{$z}}, Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            }
            }
            });
    my $grow_d = scale($config->infill_extrusion_width)/2;
    my $layer0_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.2} } ]);
    my $layer1_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.4} } ]);
    my $diff = diff($layer0_infill, $layer1_infill);
    $diff = offset2_ex($diff, -$grow_d, +$grow_d);
    $diff = [ grep { $_->area > 2*(($grow_d*2)**2) } @$diff ];
    is scalar(@$diff), 0, 'no missing parts in solid shell when fill_density is 0';
}

{
    # GH: #2697
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('perimeter_extrusion_width', 0.72);
    $config->set('top_infill_extrusion_width', 0.1);
    $config->set('infill_extruder', 2);         # in order to distinguish infill
        $config->set('solid_infill_extruder', 2);   # in order to distinguish infill

        my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my %other  = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            my $z = 1 * $self->Z;
            my $line = Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            if ($tool == $config->infill_extruder-1) {
            $infill{$z} //= [];
            push @{$infill{$z}}, $line;
            } else {
            $other{$z} //= [];
            push @{$other{$z}}, $line;
            }
            }
            });
    my $top_z = max(keys %infill);
    my $top_infill_grow_d = scale($config->top_infill_extrusion_width)/2;
    my $top_infill = union([ map @{$_->grow($top_infill_grow_d)}, @{ $infill{$top_z} } ]);
    my $perimeters_grow_d = scale($config->perimeter_extrusion_width)/2;
    my $perimeters = union([ map @{$_->grow($perimeters_grow_d)}, @{ $other{$top_z} } ]);
    my $covered = union_ex([ @$top_infill, @$perimeters ]);
    my @holes = map @{$_->holes}, @$covered;
    ok sum(map unscale unscale $_->area*-1, @holes) < 1, 'no gaps between top solid infill and perimeters';
}
*/

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle, double density)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
	filler->bounding_box = get_extents(expolygon.contour);
    filler->angle = float(angle);

	Flow flow(float(flow_spacing), 0.4f, float(flow_spacing));
	filler->spacing = flow.spacing();

	FillParams fill_params;
	fill_params.density = float(density);
	fill_params.dont_adjust = false;

	Surface surface(stBottom, expolygon);
	Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);

    // check whether any part was left uncovered
    Polygons grown_paths;
    grown_paths.reserve(paths.size());

    // figure out what is actually going on here re: data types
    float line_offset = float(scale_(filler->spacing / 2.0 + EPSILON));
    std::for_each(paths.begin(), paths.end(), [line_offset, &grown_paths] (const Slic3r::Polyline& p) {
        polygons_append(grown_paths, offset(p, line_offset));
    });

	// Shrink the initial expolygon a bit, this simulates the infill / perimeter overlap that we usually apply.
    ExPolygons uncovered = diff_ex(offset(expolygon, - float(0.2 * scale_(flow_spacing))), grown_paths, ApplySafetyOffset::Yes);

    // ignore very small dots
    const double scaled_flow_spacing = std::pow(scale_(flow_spacing), 2);
    uncovered.erase(std::remove_if(uncovered.begin(), uncovered.end(), [scaled_flow_spacing](const ExPolygon& poly) { return poly.area() < scaled_flow_spacing; }), uncovered.end());

#if 0
	if (! uncovered.empty()) {
		BoundingBox bbox = get_extents(expolygon.contour);
		bbox.merge(get_extents(uncovered));
		bbox.merge(get_extents(grown_paths));
		SVG svg("c:\\data\\temp\\test_if_solid_surface_filled.svg", bbox);
		svg.draw(expolygon);
		svg.draw(uncovered, "red");
		svg.Close();
	}
#endif

    return uncovered.empty(); // solid surface is fully filled
}

TEST_CASE("Sparse plane-path anchors match the printed infill", "[Fill][InternalBridge][Regression]")
{
    // Orca: Compare generated anchors with actual extrusion across plane-path patterns,
    // multiline and rotations; an origin shift must not pass as valid support.
    // Ultra has no sparse_infill_smooth_factor / separated_infills, so those
    // dimensions from OrcaSlicer#15206 are omitted.
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral", "archimedeanchords");
    const int multiline = GENERATE(1, 2);
    const bool rotated = GENERATE(false, true);
    CAPTURE(pattern, multiline, rotated);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "15%"},
                                   {"fill_multiline", multiline},
                                   {"infill_direction", 45},
                                   {"sparse_infill_rotate_template", rotated ? "0,25,50" : ""},
                                   {"align_infill_direction_to_model", rotated},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"resolution", 0.012}});
    Print print;
    Model model;
    Slic3r::Test::init_print({make_cube(30, 24, 1)}, print, model, config, false);
    if (rotated) {
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(23.)));
        print.apply(model, config);
    }
    print.process();

    const Layer &layer = *print.objects().front()->get_layer(4);
    Polylines printed;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                entity->collect_polylines(printed);
    REQUIRE_FALSE(printed.empty());
    const AABBTreeLines::LinesDistancer<Line> printed_tree(to_lines(printed));

    // Orca: Exclude perimeter connections: anchoring and extrusion can trim those differently.
    const Polylines anchors = intersection_pl(layer.generate_sparse_infill_polylines_for_anchoring(nullptr, nullptr, nullptr),
                                              shrink(to_polygons(layer.lslices), scale_(3.)));
    REQUIRE_FALSE(anchors.empty());
    double max_distance = 0.;
    for (const Polyline &path : anchors)
        for (const Point &point : path.equally_spaced_points(scale_(0.25)))
            max_distance = std::max(max_distance, printed_tree.distance_from_lines<false>(point));
    // Orca: Allow only the configured simplification tolerance; infill-scale offsets
    // would hide anchors that no longer coincide with printed lines.
    CHECK(unscale<double>(max_distance) <= config.opt_float("resolution"));
}

TEST_CASE("Locked Zag bands fall back for patterns that need per-object state", "[Fill][LockedZag]")
{
    // adaptivecubic, supportcubic and lightning are left out of the locked_sk*_infill_pattern menus
    // because their fillers need an octree / generator that is only built when a region's own sparse
    // pattern asks for one. A stored value can still carry them (a Bambu preset lists them; the GUI
    // combo stored its row index as the value before the key mapping), so slicing must not
    // dereference the missing state - the band keeps the Locked Zag filler's own pattern instead.
    const std::string pattern = GENERATE("adaptivecubic", "supportcubic", "lightning");
    const bool        in_skin = GENERATE(false, true);
    CAPTURE(pattern, in_skin);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", "lockedzag"},
                                   {"sparse_infill_density", "20%"},
                                   {"locked_skin_infill_pattern", in_skin ? pattern : std::string("default")},
                                   {"locked_skeleton_infill_pattern", in_skin ? std::string("default") : pattern},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});
    Print print;
    Model model;
    Slic3r::Test::init_print({make_cube(30, 30, 6)}, print, model, config, false);
    print.process();

    const Layer &layer = *print.objects().front()->get_layer(10);
    Polylines printed;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                entity->collect_polylines(printed);
    CHECK_FALSE(printed.empty());
}

// Slices a 30x30x6 mm cube at 0.2 mm layers (layers 0..29) with the given surface settings.
static void process_surface_density_cube(Print &print, Model &model, std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"top_shell_layers", 4},
                                   {"bottom_shell_layers", 3},
                                   {"sparse_infill_density", "15%"}});
    config.set_deserialize_strict(items);
    Slic3r::Test::init_print({make_cube(30, 30, 6)}, print, model, config, false);
    print.process();
}

static Polylines fill_polylines(const Layer &layer, ExtrusionRole role)
{
    Polylines out;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == role)
                entity->collect_polylines(out);
    return out;
}

static double fill_length_mm(const Polylines &polylines)
{
    return std::accumulate(polylines.begin(), polylines.end(), 0.,
                           [](double acc, const Polyline &pl) { return acc + unscale<double>(pl.length()); });
}

TEST_CASE("Top and bottom surface density", "[Fill][SurfaceDensity]")
{
    Print print;
    Model model;

    SECTION("0% top surface density prints only the walls on the top layer") {
        // Fillers divide their line spacing by the density, so a 0% top surface must be dropped
        // before it reaches them rather than filled with an infinite spacing.
        process_surface_density_cube(print, model, {{"top_surface_density", "0%"}});
        const Layer &top = *print.objects().front()->get_layer(29);
        CHECK(fill_polylines(top, erTopSolidInfill).empty());
        CHECK_FALSE(top.regions().front()->perimeters.entities.empty());
    }

    SECTION("A lower top surface density spaces the top lines apart") {
        process_surface_density_cube(print, model, {{"top_surface_density", "100%"}});
        const double full = fill_length_mm(fill_polylines(*print.objects().front()->get_layer(29), erTopSolidInfill));
        Print print_half;
        Model model_half;
        process_surface_density_cube(print_half, model_half, {{"top_surface_density", "50%"}});
        const double half = fill_length_mm(fill_polylines(*print_half.objects().front()->get_layer(29), erTopSolidInfill));
        CAPTURE(full, half);
        REQUIRE(full > 0.);
        CHECK(half > 0.3 * full);
        CHECK(half < 0.75 * full);
    }

    SECTION("A lower bottom surface density spaces the bottom lines apart") {
        process_surface_density_cube(print, model, {{"bottom_surface_density", "100%"}});
        const double full = fill_length_mm(fill_polylines(*print.objects().front()->get_layer(0), erBottomSurface));
        Print print_half;
        Model model_half;
        process_surface_density_cube(print_half, model_half, {{"bottom_surface_density", "50%"}});
        const double half = fill_length_mm(fill_polylines(*print_half.objects().front()->get_layer(0), erBottomSurface));
        CAPTURE(full, half);
        REQUIRE(full > 0.);
        CHECK(half > 0.3 * full);
        CHECK(half < 0.75 * full);
    }
}

TEST_CASE("Undertop surface pattern fills the solid layer under a sparse top", "[Fill][SurfaceDensity]")
{
    // Share of the printed length running parallel to the cube's X or Y edges. Concentric rings on a
    // square are axis-aligned; the default monotonic solid infill runs at 45 degrees.
    auto axis_aligned_share = [](const Polylines &polylines) {
        double aligned = 0., total = 0.;
        for (const Polyline &pl : polylines)
            for (const Line &line : pl.lines()) {
                const Vec2d  d   = (line.b - line.a).cast<double>();
                const double len = d.norm();
                total += len;
                if (std::min(std::abs(d.x()), std::abs(d.y())) < 0.1 * len)
                    aligned += len;
            }
        return total > 0. ? aligned / total : 0.;
    };

    const std::string undertop = GENERATE("default", "concentric");
    const std::string density  = GENERATE("50%", "100%");
    CAPTURE(undertop, density);

    Print print;
    Model model;
    process_surface_density_cube(print, model, {{"top_surface_density", density}, {"undertop_surface_pattern", undertop}});
    // Layer 28 is the solid layer directly under the top skin on layer 29.
    const Polylines under_top = fill_polylines(*print.objects().front()->get_layer(28), erSolidInfill);
    REQUIRE_FALSE(under_top.empty());
    const double share = axis_aligned_share(under_top);
    CAPTURE(share);
    // The undertop pattern only takes over while the top surface is sparse enough to show it.
    if (undertop == "concentric" && density == "50%")
        CHECK(share > 0.8);
    else
        CHECK(share < 0.5);
}

TEST_CASE("Gyroid infill of an object matches the infill of a larger object with the same center", "[Fill]")
{
    // Orca #16002: both the parametric waves and (since the #13379 port) the optimized
    // marching-squares branch. DensityAdjust and AABBTreeLines::LinesDistancer both exist on Edge;
    // the test uses them as upstream does.
    const bool   optimized = GENERATE(false, true);
    const int    multiline = GENERATE(1, 2);
    const float  density   = GENERATE(0.05f, 0.2f);
    const double spacing   = 0.45;
    CAPTURE(optimized, multiline, density);

    auto circle = [](double radius) {
        Polygon contour = make_circle_num_segments(scale_(radius), 120);
        contour.translate(Point::new_scale(100., 60.));
        return ExPolygon(std::move(contour));
    };
    const ExPolygon object = circle(20.);
    const ExPolygon larger = circle(30.);
    auto fill = [optimized, multiline, density, spacing](const ExPolygon &region, double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipGyroid));
        filler->spacing = spacing;
        filler->angle   = float(M_PI / 7.);
        filler->z       = z;

        FillParams params;
        params.density          = density;
        params.multiline        = multiline;
        params.gyroid_optimized = optimized;
        params.dont_adjust      = true;
        Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary of the object, where both are clipped and connected the same way.
    const Polygons inner = shrink(to_polygons(object), scale_(1.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };

    // Multiline 1 reproduces the larger object's waves to 10 um. With multiline > 1 the interleaved
    // waves are generated per bounding box (the accepted G-code change of this PR for multiline
    // 2-5: the result can move slightly with the bbox), so two bboxes of the same center differ a
    // little more: 12.6 um at multiline 2, density 0.05, z=17.38 on MSVC (deterministic), far below
    // the 0.45 mm line spacing. A real phase shift is a large fraction of the wave period (the
    // pinned multiline-1 test below catches that), so 20 um stays meaningful. Multiline 1 keeps 10 um.
    // Marching squares (the optimized branch) simplifies rings that start elsewhere in each object,
    // so it gets upstream's SPARSE_INFILL_RESOLUTION + 10 um. With multiline 2 our multiline_fill offsets
    // those simplified rings per bounding box (see above), which moved a copy by 82 um at density 0.05,
    // z=2.897 on MSVC: still under a fifth of the 0.45 mm spacing, so 0.1 mm is allowed there.
    const double tolerance = optimized ? (multiline > 1 ? 0.1 : SPARSE_INFILL_RESOLUTION + 0.01) : (multiline > 1 ? 0.02 : 0.01);
    // Half a z period of the waves, through both switches between horizontal and vertical waves.
    const double wave_distance = spacing * multiline / (density * FillGyroid::DensityAdjust);
    for (int step = 0; step <= 8; ++step) {
        const double z = wave_distance * M_PI * step / 8.;
        CAPTURE(z);
        const Polylines paths = fill(object, z);
        REQUIRE_FALSE(paths.empty());
        const Polylines reference = fill(larger, z);
        CHECK(farthest(reference, paths) < tolerance);
        CHECK(farthest(paths, reference) < tolerance);
    }
}

TEST_CASE("Gyroid multiline-1 waves stay pinned and cover the contour edge", "[Fill]")
{
    // The same-center case above shrinks 1 mm inward, so it stays green on main and
    // would miss a global phase shift or a dropped strip at the bbox edge. These
    // pins are world-mm vertices of the phase-preserving generator (multiline 1,
    // density 0.2, spacing 0.45, angle = π/4 so CorrectionAngle cancels).
    // fill_surface insets the 10..50 mm square by 0.5*spacing first (overlap 0),
    // so the filled region is 10.225..49.775. At z=0 the waves run along Y,
    // ~2.90 mm apart; the last kept wave spans x=46.921..48.370 and the next
    // (49.818..51.266) is clipped, so min_right is 1.6305 mm from x=50, not 0.225.
    const double spacing = 0.45;
    const float  density = 0.2f;
    FillParams params;
    params.density           = density;
    params.multiline         = 1;
    params.dont_adjust       = true;
    params.anchor_length     = 0.f;
    params.anchor_length_max = 0.f; // dont_connect: keep wave vertices unjoined

    Polygon square{
        Point::new_scale(10., 10.), Point::new_scale(50., 10.),
        Point::new_scale(50., 50.), Point::new_scale(10., 50.)
    };
    auto fill_at = [&](double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipGyroid));
        filler->spacing = spacing;
        filler->angle   = float(M_PI / 4.);
        filler->z       = z;
        Surface surface(stInternal, ExPolygon(square));
        return filler->fill_surface(&surface, params);
    };
    auto pin_ok = [](const Polylines &paths, double x, double y, double tol) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(paths));
        const Point q = Point::new_scale(x, y);
        const double d = unscale<double>(tree.distance_from_lines<false>(q));
        CAPTURE(x, y, d);
        CHECK(d < tol);
    };

    const Polylines paths0 = fill_at(0.);
    REQUIRE_FALSE(paths0.empty());
    const double pin_tol = 0.02;
    // z=0: waves run along Y. A shifted origin moves these by millimetres.
    const double pins_y[][2] = {
        {13.449314, 33.884724},
        {15.211169, 13.606001},
        {24.469603, 30.263524},
        {30.263524, 30.263524},
        {41.851366, 30.263524},
        {46.921046, 31.712004},
    };
    for (const auto &xy : pins_y)
        pin_ok(paths0, xy[0], xy[1], pin_tol);

    // Reach to the original 10..50 mm sides after the 0.5*spacing inset + clip.
    // Pin the four values (tol 0.02): a dropped strip or a looser 1 mm gate on
    // the sparse +X side would miss the real 1.6305 mm right reach.
    double min_left = 1e9, min_right = 1e9, min_bottom = 1e9, min_top = 1e9;
    for (const Polyline &pl : paths0)
        for (const Point &p : pl.points) {
            const double x = unscale<double>(p.x());
            const double y = unscale<double>(p.y());
            min_left   = std::min(min_left,   std::abs(x - 10.));
            min_right  = std::min(min_right,  std::abs(x - 50.));
            min_bottom = std::min(min_bottom, std::abs(y - 10.));
            min_top    = std::min(min_top,    std::abs(y - 50.));
        }
    CAPTURE(min_left, min_right, min_bottom, min_top);
    CHECK(std::abs(min_left   - 0.225)  < 0.02);
    CHECK(std::abs(min_right  - 1.6305) < 0.02);
    CHECK(std::abs(min_bottom - 0.225)  < 0.02);
    CHECK(std::abs(min_top    - 0.225)  < 0.02);

    // Second pin set: z such that the pattern angle is π/2, so waves run along X.
    // At z=0 some X shifts look identical; these Y-separated vertices would not.
    const double z_along_x = (M_PI / 2.) * spacing / (density * FillGyroid::DensityAdjust);
    const Polylines pathsX = fill_at(z_along_x);
    REQUIRE_FALSE(pathsX.empty());
    const double pins_x[][2] = {
        {30.263524, 15.054482},
        {30.263524, 29.539284},
        {13.606001, 29.382597},
        {30.263524, 41.127126},
    };
    for (const auto &xy : pins_x)
        pin_ok(pathsX, xy[0], xy[1], pin_tol);
}

// Ironing path count and total length in mm, over the object's fills and its support fills.
static std::pair<size_t, double> ironing_extent(const Print &print)
{
    size_t paths  = 0;
    double length = 0.;
    auto   accumulate = [&](const ExtrusionEntityCollection &fills) {
        const ExtrusionEntityCollection flat = fills.flatten();
        for (const ExtrusionEntity *entity : flat.entities)
            if (entity->role() == erIroning) {
                ++paths;
                length += unscale<double>(entity->length());
            }
    };
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            accumulate(region->fills);
    for (const SupportLayer *support_layer : print.objects().front()->support_layers()) {
        accumulate(support_layer->support_fills);
        for (const auto &kv : support_layer->interface_by_extruder)
            accumulate(kv.second);
    }
    return {paths, length};
}

TEST_CASE("Concentric fill with zero or invalid spacing returns without hanging", "[Fill][Regression]")
{
    // CLI / 3MF can still load ironing_spacing = 0 (or a negative). The inset loop in
    // FillConcentric never shrinks the region then, so it would run forever. Tiny positive
    // values still make progress; the clamp in make_ironing / SupportParameters is what
    // turns those into IRONING_SPACING_MIN before this filler sees them.
    const double spacing = GENERATE(0., -0.1);
    const bool   arachne = GENERATE(false, true);
    CAPTURE(spacing, arachne);

    std::unique_ptr<Fill> filler(Fill::new_from_type(ipConcentric));
    filler->spacing      = spacing;
    filler->bounding_box = BoundingBox(Point(0, 0), Point::new_scale(20, 20));
    PrintConfig       print_config;
    PrintObjectConfig print_object_config;
    filler->print_config        = &print_config;
    filler->print_object_config = &print_object_config;

    FillParams params;
    params.density      = 1.f;
    params.use_arachne  = arachne;
    params.layer_height = 0.2;
    Surface surface(stTop, ExPolygon({Point(0, 0), Point::new_scale(20, 0),
                                      Point::new_scale(20, 20), Point::new_scale(0, 20)}));

    if (arachne) {
        CHECK(filler->fill_surface_arachne(&surface, params).empty());
    } else {
        CHECK(filler->fill_surface(&surface, params).empty());
    }
}

TEST_CASE("Ironing spacing of 0 is clamped", "[Fill][Ironing]")
{
    // Edge has no filament_ironing_spacing, so only the process-level option is exercised.
    const std::string pattern = GENERATE("rectilinear", "concentric");
    const double      spacing = GENERATE(0., -0.1, 0.001);
    CAPTURE(pattern, spacing);

    auto ironing_for = [&pattern](double line_spacing) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"ironing_type", "top"},
                                       {"ironing_pattern", pattern},
                                       {"ironing_spacing", line_spacing},
                                       {"layer_height", 0.2},
                                       {"initial_layer_print_height", 0.2}});
        Print print;
        Model model;
        Slic3r::Test::init_print({make_cube(20, 20, 6)}, print, model, config, false);
        print.process();
        return ironing_extent(print);
    };

    const std::pair<size_t, double> clamped = ironing_for(spacing);
    const std::pair<size_t, double> minimum = ironing_for(IRONING_SPACING_MIN);
    REQUIRE(minimum.first > 0);
    CHECK(clamped.first == minimum.first);
    CHECK(clamped.second == Approx(minimum.second));
}

TEST_CASE("Support ironing spacing of 0 is clamped", "[Fill][Ironing]")
{
    const double spacing = GENERATE(0., -0.1, 0.001);
    CAPTURE(spacing);

    auto ironing_for = [](double line_spacing) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"enable_support", "1"},
                                       {"support_type", "normal(auto)"},
                                       {"support_style", "grid"},
                                       {"support_on_build_plate_only", "0"},
                                       {"support_interface_top_layers", "2"},
                                       {"support_ironing", "1"},
                                       {"support_ironing_pattern", "concentric"},
                                       {"support_ironing_spacing", line_spacing},
                                       {"layer_height", 0.2},
                                       {"initial_layer_print_height", 0.2}});
        Print print;
        Model model;
        // init_print() calls ensure_on_bed(), so a translated cube would sit on the plate
        // and never grow supports. A short pillar with a larger slab still has overhangs.
        TriangleMesh mesh = make_cube(4., 4., 8.);
        TriangleMesh slab = make_cube(12., 12., 3.);
        slab.translate(-4.f, -4.f, 8.f);
        mesh.merge(slab);
        Slic3r::Test::init_print({mesh}, print, model, config, false);
        print.process();
        REQUIRE_FALSE(print.objects().front()->support_layers().empty());
        return ironing_extent(print);
    };

    const std::pair<size_t, double> clamped = ironing_for(spacing);
    const std::pair<size_t, double> minimum = ironing_for(IRONING_SPACING_MIN);
    REQUIRE(minimum.first > 0);
    CHECK(clamped.first == minimum.first);
    CHECK(clamped.second == Approx(minimum.second));
}

// Length-weighted dominant direction of the layer's role_wanted extrusions, whole degrees
// [0, 180), or -1 if it has none. Needs a line pattern such as monotonic or rectilinear.
template<typename RolePred> static int dominant_fill_angle(const Layer &layer, RolePred role_wanted)
{
    std::map<int, double> weight_per_degree;

    auto account = [&weight_per_degree, &role_wanted](const ExtrusionPath &path) {
        if (!role_wanted(path.role()))
            return;
        const Points &pts = path.polyline.points;
        for (size_t i = 1; i < pts.size(); ++i) {
            const double dx = double(pts[i].x() - pts[i - 1].x());
            const double dy = double(pts[i].y() - pts[i - 1].y());
            const double len = std::hypot(dx, dy);
            if (len <= 0.)
                continue;
            int deg = int(std::lround(Geometry::rad2deg(std::atan2(dy, dx)))) % 180;
            if (deg < 0)
                deg += 180;
            weight_per_degree[deg] += len;
        }
    };

    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
            if (auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                account(*path);
            else if (auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                for (const ExtrusionPath &p : multi->paths)
                    account(p);
            else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                for (const ExtrusionPath &p : loop->paths)
                    account(p);
        }

    if (weight_per_degree.empty())
        return -1;
    return std::max_element(weight_per_degree.begin(), weight_per_degree.end(),
                            [](const auto &a, const auto &b) { return a.second < b.second; })->first;
}

template<typename RolePred> static std::vector<int> angles_per_layer(const Print &print, RolePred role_wanted)
{
    std::vector<int> angles;
    for (const Layer *layer : print.objects().front()->layers())
        angles.push_back(dominant_fill_angle(*layer, role_wanted));
    return angles;
}

static bool solid_role(ExtrusionRole role) { return is_solid_infill(role) && role != erIroning; }
static bool sparse_role(ExtrusionRole role) { return role == erInternalInfill; }

TEST_CASE("Infill rotation template is unaffected by a raft", "[Fill][Regression]")
{
    // More angles than raft layers, so a raft cannot alias back to the same angle.
    const std::string template_string = GENERATE("+45", "0,25,50,75,100,125,150");
    const int raft_layers = GENERATE(1, 3);
    CAPTURE(template_string, raft_layers);

    auto angles_for = [&template_string](int rafts) {
        Print print;
        // 100% density makes every layer solid, so the template shows on all 100, not just shells.
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"solid_infill_rotate_template", template_string},
                                             {"sparse_infill_density", "100%"},
                                             {"internal_solid_infill_pattern", "monotonic"},
                                             {"layer_height", 0.2},
                                             {"raft_layers", rafts}});
        return angles_per_layer(print, solid_role);
    };

    const std::vector<int> without_raft = angles_for(0);
    const std::vector<int> with_raft    = angles_for(raft_layers);

    REQUIRE(without_raft.size() == 100);
    REQUIRE(with_raft.size() == without_raft.size());
    REQUIRE(std::count(without_raft.begin(), without_raft.end(), -1) == 0);
    CHECK(with_raft == without_raft);
}

TEST_CASE("Sparse infill rotation template turns the infill layer by layer", "[Fill]")
{
    const std::vector<int> expected_cycle = {0, 25, 50, 75, 100, 125, 150};

    Print print;
    // No shells, so every layer is sparse infill rather than solid.
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(10, 10, 10)}, print,
                                        {{"sparse_infill_rotate_template", "0,25,50,75,100,125,150"},
                                         {"sparse_infill_density", "40%"},
                                         {"sparse_infill_pattern", "rectilinear"},
                                         {"top_shell_layers", 0},
                                         {"bottom_shell_layers", 0},
                                         {"layer_height", 0.2}});

    const std::vector<int> angles = angles_per_layer(print, sparse_role);
    REQUIRE(angles.size() == 50);
    REQUIRE(std::count(angles.begin(), angles.end(), -1) == 0);

    std::vector<int> expected;
    for (size_t i = 0; i < angles.size(); ++i)
        expected.push_back(expected_cycle[i % expected_cycle.size()]);
    CHECK(angles == expected);
}

TEST_CASE("Infill rotation template layer count modifier holds each angle for N layers", "[Fill]")
{
    Print print;
    // "+45#2" turns 45 degrees every 2 layers, so equal angles come in pairs.
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(10, 10, 10)}, print,
                                        {{"solid_infill_rotate_template", "+45#2"},
                                         {"sparse_infill_density", "100%"},
                                         {"internal_solid_infill_pattern", "monotonic"},
                                         {"layer_height", 0.2}});

    const std::vector<int> angles = angles_per_layer(print, solid_role);
    REQUIRE(angles.size() == 50);
    REQUIRE(std::count(angles.begin(), angles.end(), -1) == 0);

    std::vector<int> run_lengths;
    for (size_t i = 0; i < angles.size();) {
        size_t j = i;
        while (j < angles.size() && angles[j] == angles[i])
            ++j;
        run_lengths.push_back(int(j - i));
        i = j;
    }
    // The first and last runs can be clipped by the start and end of the object.
    REQUIRE(run_lengths.size() > 3);
    const std::vector<int> interior(run_lengths.begin() + 1, run_lengths.end() - 1);
    CHECK(std::count(interior.begin(), interior.end(), 2) == int(interior.size()));
}

TEST_CASE("Z anti-aliasing keeps the infill rotation template's step", "[Fill]")
{
    Print print;
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(10, 10, 10)}, print,
                                        {{"solid_infill_rotate_template", "+45"},
                                         {"sparse_infill_density", "100%"},
                                         {"internal_solid_infill_pattern", "monotonic"},
                                         {"zaa_enabled", 1},
                                         {"zaa_min_z", 0.05},
                                         {"layer_height", 0.2}});

    // Z contouring varies the layer heights, so the layer count is not 10mm / 0.2mm here.
    const std::vector<int> angles = angles_per_layer(print, solid_role);
    REQUIRE(angles.size() > 10);
    REQUIRE(std::count(angles.begin(), angles.end(), -1) == 0);

    // Z contouring may change when the template advances, but each step must still be 45 degrees.
    int steps = 0;
    for (size_t i = 1; i < angles.size(); ++i) {
        const int delta = ((angles[i] - angles[i - 1]) % 180 + 180) % 180;
        CAPTURE(i, angles[i - 1], angles[i]);
        // Split rather than "delta == 0 || delta == 45" so Catch2 can show the operands.
        REQUIRE(delta % 45 == 0);
        REQUIRE(delta <= 45);
        steps += delta == 45;
    }
    CHECK(steps > 0);
}

TEST_CASE("Solid infill direction offsets every layer when no template is set", "[Fill]")
{
    auto angles_for = [](int direction) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(10, 10, 10)}, print,
                                            {{"solid_infill_direction", direction},
                                             {"sparse_infill_density", "100%"},
                                             {"internal_solid_infill_pattern", "monotonic"},
                                             {"layer_height", 0.2}});
        return angles_per_layer(print, solid_role);
    };

    const std::vector<int> at_0  = angles_for(0);
    const std::vector<int> at_30 = angles_for(30);
    REQUIRE(at_0.size() == at_30.size());
    REQUIRE(std::count(at_0.begin(), at_0.end(), -1) == 0);

    for (size_t i = 0; i < at_0.size(); ++i) {
        const int delta = ((at_30[i] - at_0[i]) % 180 + 180) % 180;
        CAPTURE(i, at_0[i], at_30[i]);
        CHECK(delta == 30);
    }
}

// Orca #11526 / #13523: the elephant foot layers density thins the internal solid infill of the layers above the
// first one, up to elefant_foot_compensation_layers; the second layer gets the set density and the following
// layers get linearly denser.
TEST_CASE("Elephant foot layers density thins the solid layers above the first one", "[Fill][ElefantFootDensity]")
{
    // Six bottom shell layers: layers 1..5 are internal solid infill, layer 0 the bottom surface.
    auto solid_lengths = [](std::initializer_list<ConfigBase::SetDeserializeItem> items) {
        Print print;
        Model model;
        process_surface_density_cube(print, model, items);
        const PrintObject &object = *print.objects().front();
        std::vector<double> lengths;
        lengths.push_back(fill_length_mm(fill_polylines(*object.get_layer(0), erBottomSurface)));
        for (int layer = 1; layer <= 5; ++layer)
            lengths.push_back(fill_length_mm(fill_polylines(*object.get_layer(layer), erSolidInfill)));
        return lengths;
    };

    SECTION("The default is 100% and changes nothing") {
        const auto config = DynamicPrintConfig::full_print_config();
        REQUIRE(config.option<ConfigOptionPercent>("elefant_foot_layers_density") != nullptr);
        CHECK(config.option<ConfigOptionPercent>("elefant_foot_layers_density")->value == Approx(100.));

        const std::vector<double> unset         = solid_lengths({{"bottom_shell_layers", 6}, {"elefant_foot_compensation_layers", 3}});
        const std::vector<double> explicit_full = solid_lengths({{"bottom_shell_layers", 6}, {"elefant_foot_compensation_layers", 3},
                                                                 {"elefant_foot_layers_density", "100%"}});
        CHECK(unset == explicit_full);
    }

    SECTION("80% over three layers: 80%, 86.7%, 93.3%, then full density") {
        const std::vector<double> full    = solid_lengths({{"bottom_shell_layers", 6}, {"elefant_foot_compensation_layers", 3}});
        const std::vector<double> thinned = solid_lengths({{"bottom_shell_layers", 6}, {"elefant_foot_compensation_layers", 3},
                                                           {"elefant_foot_layers_density", "80%"}});
        REQUIRE(full.size() == 6);
        REQUIRE(thinned.size() == 6);
        for (double length : full)
            REQUIRE(length > 0.);
        // The bottom surface and the layers above the compensation layers are untouched.
        CHECK(thinned[0] == Approx(full[0]));
        CHECK(thinned[4] == Approx(full[4]));
        CHECK(thinned[5] == Approx(full[5]));
        // Fillers divide their line spacing by the density, so the printed length scales with it.
        const double expected[] = {0.8, 1. - 0.2 * 2. / 3., 1. - 0.2 / 3.};
        for (int layer = 1; layer <= 3; ++layer) {
            const double ratio = thinned[layer] / full[layer];
            CAPTURE(layer, ratio, expected[layer - 1]);
            CHECK(ratio == Approx(expected[layer - 1]).margin(0.05));
        }
        // #13523: denser going up, not sparser.
        CHECK(thinned[1] < thinned[2]);
        CHECK(thinned[2] < thinned[3]);
    }

    SECTION("EdgeSlicer: not applied on a raft, like the elephant foot compensation itself") {
        const std::vector<double> full    = solid_lengths({{"bottom_shell_layers", 6}, {"elefant_foot_compensation_layers", 3},
                                                           {"raft_layers", 2}});
        const std::vector<double> thinned = solid_lengths({{"bottom_shell_layers", 6}, {"elefant_foot_compensation_layers", 3},
                                                           {"raft_layers", 2}, {"elefant_foot_layers_density", "80%"}});
        CHECK(thinned == full);
    }
}

// Orca: the spiral inset pattern chains the concentric loops into a single continuous path per
// island, so it has to cope with the degenerate loops offsetting leaves behind and it must not join
// loops that only look adjacent. (Ported from OrcaSlicer #15085.)
namespace {

Slic3r::Polylines spiral_inset_fill(const Slic3r::ExPolygon &surface_shape, double spacing, float density = 1.f)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("spiralinset"));
    filler->spacing = spacing;
    // Cancel the half-spacing contraction fill_surface() applies, so the filler sees the shape as given.
    filler->overlap = 0.5 * spacing;

    Slic3r::FillParams fill_params;
    fill_params.density     = density;
    fill_params.dont_adjust = true;

    Slic3r::Surface surface(Slic3r::stBottom, surface_shape);
    return filler->fill_surface(&surface, fill_params);
}

Slic3r::ExPolygon spiral_rectangle(double x, double y, double w, double h)
{
    return Slic3r::ExPolygon({Slic3r::Point::new_scale(x, y), Slic3r::Point::new_scale(x + w, y),
                              Slic3r::Point::new_scale(x + w, y + h), Slic3r::Point::new_scale(x, y + h)});
}

// Area of the surface the toolpaths fail to cover, and the largest single patch of it, in mm2. Each
// bead is measured at its own width so the variable width walls are not sold short.
std::pair<double, double> spiral_uncovered_area(const Slic3r::ExPolygon &surface_shape, const Slic3r::Polygons &covered)
{
    double total = 0, biggest = 0;
    for (const Slic3r::ExPolygon &gap : Slic3r::diff_ex(Slic3r::ExPolygons{surface_shape}, Slic3r::union_(covered))) {
        const double area = unscale<double>(unscale<double>(gap.area()));
        total += area;
        biggest = std::max(biggest, area);
    }
    return {total, biggest};
}

Slic3r::Polygons spiral_beads_of(const Slic3r::Polylines &paths, double width)
{
    return Slic3r::offset(paths, float(scale_(0.5 * width)));
}

Slic3r::Polygons spiral_beads_of(const Slic3r::ThickPolylines &paths)
{
    Slic3r::Polygons covered;
    for (const Slic3r::ThickPolyline &path : paths)
        for (size_t i = 0; i + 1 < path.points.size(); ++i) {
            Slic3r::Polyline segment;
            segment.points = {path.points[i], path.points[i + 1]};
            Slic3r::append(covered, Slic3r::offset(Slic3r::Polylines{segment},
                                                  float(0.5 * std::max(path.width[2 * i], path.width[2 * i + 1]))));
        }
    return covered;
}

} // namespace

TEST_CASE("Spiral inset fill drops loops shorter than the end clipping", "[Fill][SpiralInset][Regression]")
{
    // A sliver whose whole perimeter is shorter than the length clipped off the end of a loop, so the
    // clipping consumes the path entirely. Such a loop carries no extrusion and must be dropped
    // rather than kept as an empty path and read back from.
    const double spacing = 0.45;

    Slic3r::Polylines paths;
    REQUIRE_NOTHROW(paths = spiral_inset_fill(spiral_rectangle(0, 0, 0.05, 0.05), spacing));
    for (const Slic3r::Polyline &path : paths)
        CHECK(path.size() >= 2);

    // The same surface at a size the clipping cannot swallow still gets filled.
    REQUIRE_NOTHROW(paths = spiral_inset_fill(spiral_rectangle(0, 0, 5, 5), spacing));
    REQUIRE(paths.size() == 1);
    CHECK(paths.front().size() >= 2);
}

TEST_CASE("Spiral inset fill keeps separate islands on separate paths", "[Fill][SpiralInset]")
{
    // Two lobes joined by a neck narrower than the loop spacing: the inward offsets break the surface
    // into two islands, which cannot share one spiral, and no path may leave the surface.
    const double spacing = 0.45;
    Slic3r::ExPolygon dumbbell = spiral_rectangle(0, 0, 6, 6);
    dumbbell = Slic3r::union_ex(Slic3r::ExPolygons{dumbbell, spiral_rectangle(6, 2.9, 4, 0.2), spiral_rectangle(10, 0, 6, 6)}).front();

    const Slic3r::Polylines paths = spiral_inset_fill(dumbbell, spacing);
    REQUIRE(paths.size() >= 2);

    // Inflate by a hair so that loops sitting exactly on the outline still count as contained.
    const Slic3r::ExPolygons within = Slic3r::offset_ex(dumbbell, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);
    for (const Slic3r::Polyline &path : paths) {
        CHECK(path.size() >= 2);
        CHECK(within.front().contains(path));
    }
}

TEST_CASE("Spiral inset fill stays connected across sharp corners", "[Fill][SpiralInset][Regression]")
{
    // At a corner of half-angle a, the next ring inward retreats along the bisector by spacing/sin(a),
    // which leaves it several spacings from the end of the ring it continues. Judging the break by
    // distance broke the spiral into loose rings at every spike; nesting is what decides the island.
    const double spacing = 0.45;
    const Slic3r::ExPolygon spike({Slic3r::Point::new_scale(0, 0), Slic3r::Point::new_scale(30, 0),
                                   Slic3r::Point::new_scale(15, 4)});

    const Slic3r::Polylines paths = spiral_inset_fill(spike, spacing);
    CHECK(paths.size() == 1);

    const Slic3r::ExPolygons within = Slic3r::offset_ex(spike, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);
    for (const Slic3r::Polyline &path : paths)
        CHECK(within.front().contains(path));
}

TEST_CASE("Spiral inset fill starts on a convex corner", "[Fill][SpiralInset][Regression]")
{
    // The only right angle on this outline is the reflex one: the two edges meeting at the origin
    // span 90 degrees exactly as a square corner would, but the material lies outside them. The next
    // ring in steps away from a reflex corner along the bisector instead of hugging it, so starting
    // the spiral there sent it across a long diagonal on every single ring.
    const double spacing = 0.45;
    const Slic3r::ExPolygon notched({Slic3r::Point::new_scale(0, 0), Slic3r::Point::new_scale(0, 10),
                                     Slic3r::Point::new_scale(-16, 18), Slic3r::Point::new_scale(-16, -2),
                                     Slic3r::Point::new_scale(-8, -16), Slic3r::Point::new_scale(18, -16),
                                     Slic3r::Point::new_scale(10, 0)});

    const Slic3r::Polylines paths = spiral_inset_fill(notched, spacing);
    REQUIRE(paths.size() >= 1);

    // Every edge of the outline is at least 45 degrees off the bisector of that reflex corner, and
    // so is every ring offset from it. A long segment running along the bisector can therefore only
    // be the spiral striking out across the rings to reach the next one.
    for (const Slic3r::Polyline &path : paths)
        for (const Slic3r::Line &segment : path.lines()) {
            const Vec2d  v         = (segment.b - segment.a).cast<double>();
            const double direction = std::fmod(std::atan2(v.y(), v.x()) * 180.0 / PI + 180.0, 180.0);
            if (std::abs(direction - 45.0) > 25.0)
                continue;
            CAPTURE(direction, unscale<double>(segment.length()));
            CHECK(segment.length() <= scale_(1.5 * spacing));
        }
}

TEST_CASE("Spiral inset fill closes the gaps with variable width walls", "[Fill][SpiralInset]")
{
    // Fixed width loops cannot fill a region that is not a whole number of lines across and leave the
    // remainder open, which on a ring shows up as a wedge several lines wide. Plain concentric avoids
    // that by building solid surfaces out of Arachne's variable width walls, and so must this pattern.
    const double spacing = 0.45;
    Slic3r::ExPolygon ring = spiral_rectangle(0, 0, 24, 24);
    Slic3r::Polygon   hole;
    for (int i = 0; i < 64; ++i) {
        const double angle = -2.0 * PI * i / 64.0; // clockwise, so it reads as a hole
        hole.points.emplace_back(Slic3r::Point::new_scale(12 + 7.3 * std::cos(angle), 12 + 7.3 * std::sin(angle)));
    }
    ring.holes.emplace_back(hole);

    Slic3r::PrintConfig       print_config;
    Slic3r::PrintObjectConfig object_config;
    auto make_filler = [&]() {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("spiralinset"));
        filler->spacing             = spacing;
        filler->overlap             = 0.5 * spacing; // cancel the contraction, so both see the same surface
        filler->print_config        = &print_config;
        filler->print_object_config = &object_config;
        return filler;
    };

    Slic3r::FillParams params;
    params.density      = 1.f;
    params.dont_adjust  = false;
    params.layer_height = 0.2;

    const Slic3r::Surface surface(Slic3r::stTop, ring);

    std::unique_ptr<Slic3r::Fill> fixed = make_filler();
    const Slic3r::Polylines fixed_width = fixed->fill_surface(&surface, params);
    REQUIRE(!fixed_width.empty());
    const auto fixed_gaps = spiral_uncovered_area(ring, spiral_beads_of(fixed_width, fixed->spacing));

    params.use_arachne = true;
    std::unique_ptr<Slic3r::Fill> variable = make_filler();
    const Slic3r::ThickPolylines variable_width = variable->fill_surface_arachne(&surface, params);
    REQUIRE(!variable_width.empty());
    const auto variable_gaps = spiral_uncovered_area(ring, spiral_beads_of(variable_width));

    CAPTURE(fixed_gaps.first, fixed_gaps.second, variable_gaps.first, variable_gaps.second);
    // The wedges the fixed width loops leave behind are what the variable width walls take up.
    CHECK(variable_gaps.second < 0.5 * fixed_gaps.second);
    CHECK(variable_gaps.first < fixed_gaps.first);

    // And the walls are still chained into spirals rather than printed one path per wall. The ring is
    // at its narrowest (12 - 7.3) mm across and is filled from both sides, so it is at least this many
    // walls thick there and thicker elsewhere. Arachne's short thin feature walls cannot join a spiral,
    // so only the substantial paths count towards this.
    const size_t walls_across = size_t(2.0 * (12.0 - 7.3) / spacing);
    size_t       spirals      = 0;
    for (const Slic3r::ThickPolyline &path : variable_width)
        if (path.length() > scale_(10.0 * spacing))
            ++spirals;
    CAPTURE(spirals, walls_across, variable_width.size(), fixed_width.size());
    CHECK(2 * spirals < walls_across);
}

// EdgeSlicer: the top/bottom surface fill order options (OrcaSlicer #14179) are not ported, so the
// spiral always runs the way upstream's "Default" order does: outward, from the centre to the edge.
TEST_CASE("Spiral inset fill runs outward from the centre", "[Fill][SpiralInset]")
{
    const double            spacing = 0.45;
    const Slic3r::ExPolygon square  = spiral_rectangle(0, 0, 10, 10);
    const Slic3r::Polylines paths   = spiral_inset_fill(square, spacing);
    REQUIRE(paths.size() == 1);

    const Slic3r::Point centre = Slic3r::Point::new_scale(5, 5);
    const Slic3r::Polyline &spiral = paths.front();
    CAPTURE(unscale<double>(spiral.first_point().distance_to(centre)), unscale<double>(spiral.last_point().distance_to(centre)));
    // It starts near the middle and ends on the outermost ring, within a spacing of the outline.
    CHECK(spiral.first_point().distance_to(centre) < scale_(2.0 * spacing));
    CHECK(spiral.last_point().distance_to(centre) > scale_(5.0 - 1.5 * spacing));
}

TEST_CASE("Spiral inset fill is deterministic and covers a solid square", "[Fill][SpiralInset]")
{
    const double            spacing = 0.45;
    const Slic3r::ExPolygon square  = spiral_rectangle(0, 0, 20, 12);

    const Slic3r::Polylines first  = spiral_inset_fill(square, spacing);
    const Slic3r::Polylines second = spiral_inset_fill(square, spacing);
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i)
        CHECK(first[i].points == second[i].points);

    // Fixed width loops (no Arachne here) leave a sliver in the middle when the width is not a whole
    // number of lines, but the surface is still almost entirely covered.
    const auto gaps = spiral_uncovered_area(square, spiral_beads_of(first, spacing));
    CAPTURE(gaps.first, gaps.second);
    CHECK(gaps.first < 0.05 * 20. * 12.);
    CHECK(gaps.second < 0.04 * 20. * 12.);
}

TEST_CASE("Spiral inset fill at a sparse density stays inside and fills less", "[Fill][SpiralInset]")
{
    const double            spacing = 0.45;
    const Slic3r::ExPolygon square  = spiral_rectangle(0, 0, 20, 20);

    const Slic3r::Polylines solid  = spiral_inset_fill(square, spacing, 1.f);
    const Slic3r::Polylines sparse = spiral_inset_fill(square, spacing, 0.2f);
    REQUIRE(!sparse.empty());

    const Slic3r::ExPolygons within = Slic3r::offset_ex(square, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);
    double solid_length = 0, sparse_length = 0;
    for (const Slic3r::Polyline &path : solid)
        solid_length += path.length();
    for (const Slic3r::Polyline &path : sparse) {
        CHECK(within.front().contains(path));
        sparse_length += path.length();
    }
    // About a fifth of the solid length; allow slack for the outermost ring both keep.
    CAPTURE(unscale<double>(solid_length), unscale<double>(sparse_length));
    CHECK(sparse_length < 0.35 * solid_length);
    CHECK(sparse_length > 0.1 * solid_length);
}

TEST_CASE("Spiral inset internal solid infill uses plain loops, not Arachne walls", "[Fill][SpiralInset][Regression]")
{
    // OrcaSlicer #15705: Arachne's standalone thin walls could become an extra centre plug after the
    // spiral had finished, so internal solid infill keeps the fixed width loops. Top and bottom
    // surfaces still get the variable width walls.
    const double      spacing = 0.45;
    Slic3r::ExPolygon ring    = spiral_rectangle(0, 0, 24, 24);
    Slic3r::Polygon   hole;
    for (int i = 0; i < 64; ++i) {
        const double angle = -2.0 * PI * i / 64.0;
        hole.points.emplace_back(Slic3r::Point::new_scale(12 + 7.3 * std::cos(angle), 12 + 7.3 * std::sin(angle)));
    }
    ring.holes.emplace_back(hole);

    Slic3r::PrintConfig       print_config;
    Slic3r::PrintObjectConfig object_config;
    auto fill = [&](Slic3r::ExtrusionRole role) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("spiralinset"));
        filler->spacing             = spacing;
        filler->overlap             = 0.5 * spacing;
        filler->print_config        = &print_config;
        filler->print_object_config = &object_config;
        Slic3r::FillParams params;
        params.density        = 1.f;
        params.dont_adjust    = false;
        params.layer_height   = 0.2;
        params.use_arachne    = true;
        params.extrusion_role = role;
        const Slic3r::Surface surface(Slic3r::stInternalSolid, ring);
        Slic3r::ThickPolylines out = filler->fill_surface_arachne(&surface, params);
        return std::make_pair(std::move(out), filler->spacing);
    };

    const auto internal = fill(Slic3r::erSolidInfill);
    REQUIRE(!internal.first.empty());
    for (const Slic3r::ThickPolyline &path : internal.first)
        for (const coordf_t w : path.width)
            CHECK(w == Approx(scaled<coord_t>(internal.second)).margin(1.));

    // A top surface on the same shape still varies its widths.
    const auto top = fill(Slic3r::erTopSolidInfill);
    REQUIRE(!top.first.empty());
    coordf_t min_w = std::numeric_limits<coordf_t>::max(), max_w = 0;
    for (const Slic3r::ThickPolyline &path : top.first)
        for (const coordf_t w : path.width) {
            min_w = std::min(min_w, w);
            max_w = std::max(max_w, w);
        }
    CHECK(max_w - min_w > scale_(0.01));
}

// OrcaSlicer #13379: the optional "Z-buckling bias" gyroid. Our FillGyroid is our own grid-anchored
// version (#221); the optimized wave is a separate branch and must leave plain gyroid alone.
namespace {

Slic3r::Polylines gyroid_fill(const Slic3r::ExPolygon &shape, double z, float density, bool optimized)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("gyroid"));
    filler->spacing = 0.45;
    filler->angle   = 0.f;
    filler->z       = z;

    Slic3r::FillParams params;
    params.density          = density;
    params.dont_adjust      = true;
    params.layer_height     = 0.2;
    params.gyroid_optimized = optimized;

    Slic3r::Surface surface(Slic3r::stInternal, shape);
    return filler->fill_surface(&surface, params);
}

double total_length_mm(const Slic3r::Polylines &paths)
{
    double length = 0;
    for (const Slic3r::Polyline &path : paths)
        length += unscale<double>(path.length());
    return length;
}

Slic3r::ExPolygon gyroid_square(double size)
{
    return Slic3r::ExPolygon({Slic3r::Point::new_scale(0, 0), Slic3r::Point::new_scale(size, 0),
                              Slic3r::Point::new_scale(size, size), Slic3r::Point::new_scale(0, size)});
}

} // namespace

TEST_CASE("Gyroid optimized flag defaults off and leaves plain gyroid unchanged", "[Fill][Gyroid]")
{
    CHECK_FALSE(Slic3r::FillParams{}.gyroid_optimized);
    CHECK_FALSE(Slic3r::DynamicPrintConfig::full_print_config().opt_bool("gyroid_optimized"));

    const Slic3r::ExPolygon square = gyroid_square(30);
    for (const double z : {0.4, 3.0, 7.2}) {
        CAPTURE(z);
        const Slic3r::Polylines a = gyroid_fill(square, z, 0.15f, false);
        const Slic3r::Polylines b = gyroid_fill(square, z, 0.15f, false);
        REQUIRE(!a.empty());
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            CHECK(a[i].points == b[i].points);
    }
}

TEST_CASE("Optimized gyroid fills the surface with about the same line length", "[Fill][Gyroid]")
{
    const Slic3r::ExPolygon  square = gyroid_square(30);
    const Slic3r::ExPolygons within = Slic3r::offset_ex(square, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);

    for (const float density : {0.1f, 0.15f, 0.3f}) {
        for (const double z : {0.4, 3.0, 7.2}) {
            CAPTURE(density, z);
            const Slic3r::Polylines standard  = gyroid_fill(square, z, density, false);
            const Slic3r::Polylines optimized = gyroid_fill(square, z, density, true);
            REQUIRE(!optimized.empty());
            for (const Slic3r::Polyline &path : optimized)
                CHECK(within.front().contains(path));

            // "Filament use is preserved": the per-slice line length stays close to plain gyroid.
            const double l_std = total_length_mm(standard), l_opt = total_length_mm(optimized);
            CAPTURE(l_std, l_opt);
            CHECK(l_opt > 0.75 * l_std);
            CHECK(l_opt < 1.25 * l_std);

            // Deterministic.
            const Slic3r::Polylines again = gyroid_fill(square, z, density, true);
            REQUIRE(again.size() == optimized.size());
            for (size_t i = 0; i < again.size(); ++i)
                CHECK(again[i].points == optimized[i].points);
        }
    }
}

// OrcaSlicer #15205: corner smoothing of more sparse infill patterns.

TEST_CASE("Honeycomb infill rounds its cell corners with the smooth factor", "[Fill][Smoothing]")
{
    // A cell whose sides are several times the line width, so that the corners have room to be rounded.
    const double spacing = 0.45;
    const double density = 0.1;
    auto         fill    = [spacing, density](double smooth_factor) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("honeycomb"));
        filler->spacing = spacing;

        FillParams params;
        params.density = float(density);
        params.dont_adjust = true;
        // Keep the fragments apart, so that only the turns of the pattern itself are measured.
        params.anchor_length_max = 0.f;
        params.smooth_factor     = smooth_factor;

        Slic3r::ExPolygon square{ Slic3r::Points{
            Point::new_scale(0., 0.), Point::new_scale(50., 0.), Point::new_scale(50., 50.), Point::new_scale(0., 50.) } };
        Slic3r::Surface surface(stInternal, square);
        return filler->fill_surface(&surface, params);
    };

    // Cosine of the sharpest turn of any of the paths, 1 meaning none of them turns at all.
    auto sharpest_turn_cosine = [](const Slic3r::Polylines &polylines) {
        double sharpest = 1.;
        for (const Polyline &polyline : polylines)
            for (size_t i = 1; i + 1 < polyline.size(); ++i) {
                const Vec2d incoming = (polyline[i] - polyline[i - 1]).cast<double>().normalized();
                const Vec2d outgoing = (polyline[i + 1] - polyline[i]).cast<double>().normalized();
                sharpest = std::min(sharpest, incoming.dot(outgoing));
            }
        return sharpest;
    };
    auto point_count = [](const Slic3r::Polylines &polylines) {
        return std::accumulate(polylines.begin(), polylines.end(), size_t(0),
                               [](size_t count, const Polyline &polyline) { return count + polyline.size(); });
    };

    const Slic3r::Polylines sharp  = fill(0.);
    const Slic3r::Polylines smooth = fill(1.);

    REQUIRE(!sharp.empty());
    REQUIRE(smooth.size() == sharp.size());
    REQUIRE(point_count(smooth) > point_count(sharp));
    // The cell corners turn by 60 degrees; smoothing replaces them by gentle curves.
    REQUIRE(sharpest_turn_cosine(sharp) < 0.6);
    REQUIRE(sharpest_turn_cosine(smooth) > 0.9);
}

// Point count, number of turns sharper than 25 degrees and length of the sparse infill of a print.
// A rounded corner is a run of much gentler turns, so smoothing shows up as fewer sharp ones.
struct SparseInfillShape {
    size_t point_count { 0 };
    size_t sharp_turns { 0 };
    size_t path_count { 0 };
    double length { 0. };
};

static SparseInfillShape sparse_infill_shape(const Print &print)
{
    SparseInfillShape shape;

    auto account = [&shape](const ExtrusionPath &path) {
        if (path.role() != erInternalInfill)
            return;
        const Points &pts = path.polyline.points;
        ++shape.path_count;
        shape.point_count += pts.size();
        for (size_t i = 1; i < pts.size(); ++i)
            shape.length += (pts[i] - pts[i - 1]).cast<double>().norm();
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const Vec2d incoming = (pts[i] - pts[i - 1]).cast<double>();
            const Vec2d outgoing = (pts[i + 1] - pts[i]).cast<double>();
            if (incoming.squaredNorm() > 0. && outgoing.squaredNorm() > 0. &&
                incoming.normalized().dot(outgoing.normalized()) < 0.9)
                ++shape.sharp_turns;
        }
    };

    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
                if (auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                    account(*path);
                else if (auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                    for (const ExtrusionPath &p : multi->paths)
                        account(p);
                else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                    for (const ExtrusionPath &p : loop->paths)
                        account(p);
            }
    return shape;
}

TEST_CASE("Lightning infill rounds the turns of its branches with the smooth factor", "[Fill][Smoothing]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "lightning"},
                                             {"sparse_infill_density", "15%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    // The branch turns are replaced by curves, which cut the corners off and take more points to
    // describe. The turns where two branches are joined into one path stay sharp.
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Concentric infill rounds its loops with the smooth factor", "[Fill][Smoothing]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "concentric"},
                                             {"sparse_infill_density", "20%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Cross hatch infill rounds its transition layers with the smooth factor", "[Fill][Smoothing]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "crosshatch"},
                                             {"sparse_infill_density", "20%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

// EdgeSlicer: upstream's "Trapezoidal grid infill rounds its corners only with more than one line"
// needs the trapezoidal multiline form from #11435 (Clipper2 multiline infill), which we do not carry.
// Here Grid is not smoothable at any line count, and the option must not change it.
TEST_CASE("Grid infill is not smoothed without the trapezoidal multiline form", "[Fill][Smoothing]")
{
    CHECK_FALSE(is_smoothable_infill_pattern(ipGrid, 1));
    CHECK_FALSE(is_smoothable_infill_pattern(ipGrid, 2));
    CHECK_FALSE(is_smoothable_infill_pattern(ipTriangles, 2));
    CHECK_FALSE(is_smoothable_infill_pattern(ipStars, 2));
    auto shape_for = [](int multiline, const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "grid"},
                                             {"sparse_infill_density", "20%"},
                                             {"fill_multiline", multiline},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };
    for (const int multiline : {1, 2}) {
        CAPTURE(multiline);
        const SparseInfillShape sharp  = shape_for(multiline, "0%");
        const SparseInfillShape smooth = shape_for(multiline, "100%");
        REQUIRE(sharp.point_count > 0);
        CHECK(smooth.point_count == sharp.point_count);
        CHECK(smooth.length == sharp.length);
    }
}

TEST_CASE("3D honeycomb infill rounds its octahedral waves with the smooth factor", "[Fill][Smoothing]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "3dhoneycomb"},
                                             {"sparse_infill_density", "20%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Smoothed concentric infill stays inside the fill region", "[Fill][Smoothing][Regression]")
{
    // The concentric loops are offsets of the fill region and are never clipped to it, so a corner
    // rounded across its boundary ends up in a hole or over a wall. Rounding cuts toward the inside of
    // the turn, which leaves the region at every corner of a hole, and in a region thinner than the
    // curve even at a corner turning inwards.
    const bool  thin_region = GENERATE(false, true);
    ExPolygon   region;
    if (thin_region) {
        // An L of two 1.2mm wide arms: cutting the corner they meet at crosses both of them.
        region = ExPolygon{ Slic3r::Points{
            Point::new_scale(0., 0.), Point::new_scale(20., 0.), Point::new_scale(20., 1.2),
            Point::new_scale(1.2, 1.2), Point::new_scale(1.2, 20.), Point::new_scale(0., 20.) } };
    } else {
        region = ExPolygon{ Slic3r::Points{ Point::new_scale(0., 0.), Point::new_scale(50., 0.),
                                            Point::new_scale(50., 50.), Point::new_scale(0., 50.) },
                            Slic3r::Points{ Point::new_scale(30., 20.), Point::new_scale(30., 30.),
                                            Point::new_scale(20., 30.), Point::new_scale(20., 20.) } };
    }
    CAPTURE(thin_region);

    auto fill = [&region](double smooth_factor) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("concentric"));
        filler->spacing = 0.45;

        FillParams params;
        params.density       = 0.1f;
        params.dont_adjust   = true;
        params.smooth_factor = smooth_factor;

        Slic3r::Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    auto point_count = [](const Slic3r::Polylines &polylines) {
        return std::accumulate(polylines.begin(), polylines.end(), size_t(0),
                               [](size_t count, const Polyline &polyline) { return count + polyline.size(); });
    };

    const Slic3r::Polylines sharp  = fill(0.);
    const Slic3r::Polylines smooth = fill(1.);
    REQUIRE(!sharp.empty());

    // Nothing leaves the fill region, which the unrounded loops already touch from the inside.
    const ExPolygons bounds = offset_ex(region, float(SCALED_EPSILON));
    REQUIRE(diff_pl(sharp, bounds).empty());
    REQUIRE(diff_pl(smooth, bounds).empty());
    // The corners that the region has room for are still rounded.
    if (!thin_region)
        REQUIRE(point_count(smooth) > point_count(sharp));
}

TEST_CASE("Smoothing multiline lightning infill keeps its outlines connected", "[Fill][Smoothing][Regression]")
{
    // With more than one line per infill wall, the branches are printed as outlines drawn around them,
    // and the outlines of branches that run close to each other merge into one. Rounding the branches
    // before those outlines are built moves them apart, which breaks the merged outlines up into
    // separate loops - many more of them, each needing its own travel move.
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "lightning"},
                                             {"sparse_infill_density", "50%"},
                                             {"fill_multiline", 2},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.path_count > 0);
    REQUIRE(smooth.path_count <= sharp.path_count);
    // The outlines are still rounded.
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
}

// OrcaSlicer #12541: 3D Honeycomb rewrite with optional filled octahedron tops (infill_complete_top).
TEST_CASE("3D honeycomb fills its octahedron tops only when asked", "[Fill][3DHoneycomb]")
{
    CHECK_FALSE(DynamicPrintConfig::full_print_config().opt_bool("infill_complete_top"));
    CHECK_FALSE(FillParams{}.infill_complete_top);

    auto shape_for = [](bool tops) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20, 20, 20)}, print,
                                            {{"sparse_infill_pattern", "3dhoneycomb"},
                                             {"sparse_infill_density", "15%"},
                                             {"infill_complete_top", tops},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };
    const SparseInfillShape open   = shape_for(false);
    const SparseInfillShape closed = shape_for(true);
    const SparseInfillShape again  = shape_for(true);
    REQUIRE(open.point_count > 0);
    CAPTURE(open.length, closed.length);
    // The tops add dense fill on the layers where the pattern turns over, and nothing else changes.
    CHECK(closed.length > open.length);
    CHECK(closed.length < 3. * open.length);
    // Deterministic.
    CHECK(again.point_count == closed.point_count);
    CHECK(again.length == closed.length);
}

TEST_CASE("3D honeycomb fills every layer inside the surface", "[Fill][3DHoneycomb]")
{
    const Slic3r::ExPolygon square({Slic3r::Point::new_scale(0, 0), Slic3r::Point::new_scale(30, 0),
                                    Slic3r::Point::new_scale(30, 30), Slic3r::Point::new_scale(0, 30)});
    const Slic3r::ExPolygons within = Slic3r::offset_ex(square, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);
    for (const bool tops : {false, true})
        for (const int multiline : {1, 2})
            for (int layer = 1; layer <= 60; ++layer) {
                std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("3dhoneycomb"));
                filler->spacing = 0.45;
                filler->z       = 0.2 * layer;
                filler->layer_id = layer;
                Slic3r::FillParams params;
                params.density             = 0.15f;
                params.dont_adjust         = true;
                params.layer_height        = 0.2;
                params.multiline           = multiline;
                params.infill_complete_top = tops;
                Slic3r::Surface surface(Slic3r::stInternal, square);
                const Slic3r::Polylines paths = filler->fill_surface(&surface, params);
                CAPTURE(tops, multiline, layer);
                REQUIRE_FALSE(paths.empty());
                for (const Slic3r::Polyline &path : paths)
                    CHECK(within.front().contains(path));
            }
}

// OrcaSlicer #16005: Adaptive TPMS.

// Length of the sparse infill of the first object over the layers with print_z in [z_min, z_max], inside clip.
static double sparse_infill_length(const Print &print, coordf_t z_min, coordf_t z_max, const Polygons &clip)
{
    Polylines polylines;
    for (const Layer *layer : print.objects().front()->layers())
        if (layer->print_z >= z_min && layer->print_z <= z_max)
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                    if (entity->role() == erInternalInfill)
                        entity->collect_polylines(polylines);
    return unscale<double>(total_length(intersection_pl(polylines, clip)));
}


// EdgeSlicer: upstream compares SparseInfillShape::sequence, which our helper does not have; these are
// the sparse infill paths of the first object, in print order.
static std::vector<Points> sparse_infill_sequence(const Print &print)
{
    std::vector<Points> sequence;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                if (entity->role() == erInternalInfill) {
                    Polylines polylines;
                    entity->collect_polylines(polylines);
                    for (Polyline &polyline : polylines)
                        sequence.emplace_back(std::move(polyline.points));
                }
    return sequence;
}

static Polygons centered_square(double half)
{
    return {Polygon({Point::new_scale(-half, -half), Point::new_scale(half, -half), Point::new_scale(half, half), Point::new_scale(-half, half)})};
}

TEST_CASE("Adaptive TPMS infill thins out from the surface to the interior density", "[Fill][TpmsAdaptive]")
{
    // The modes following the distance print more lines than their target where its levels meet: along the shells, or
    // where the patterns blend.
    const auto row = GENERATE(table<std::string, std::string, double>({{"tpmsd", "lobes", 0.5},
                                                                        {"tpmsfk", "lobes", 0.5},
                                                                        {"gyroid", "lobes", 0.5},
                                                                        {"gyroid", "stepped_shells", 0.6},
                                                                        {"tpmsfk", "smooth_blend", 0.6},
                                                                        {"tpmsd", "distance_warp", 0.5}}));
    const std::string pattern  = std::get<0>(row);
    const std::string mode     = std::get<1>(row);
    const double      max_core = std::get<2>(row);
    CAPTURE(pattern, mode);
    // A 60 mm cube in 0.4 mm layers, centered on the origin in XY. Its center is 30 mm from every face.
    auto slice = [&pattern](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(60., 60., 60.)}, print,
                                            {{"sparse_infill_pattern", pattern},
                                             {"sparse_infill_density", "25%"},
                                             {"tpms_adaptive", adaptive},
                                             {"tpms_interior_density", "5%"},
                                             {"tpms_adaptive_gradient", "linear"},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
    };
    Print uniform, adaptive;
    slice("disabled", uniform);
    slice(mode, adaptive);

    // The middle of the cube, at most 10 mm from the center: at most 5% + 20% * 10 / 30 = 11.7% dense.
    const Polygons core = centered_square(10.);
    // Along the sides at mid height, 24 to 28 mm from the center: at least 5% + 20% * 24 / 30 = 21% dense.
    const Polygons shell = diff(centered_square(28.), centered_square(24.));
    const double core_uniform = sparse_infill_length(uniform, 25., 35., core);
    const double shell_uniform = sparse_infill_length(uniform, 25., 35., shell);
    REQUIRE(core_uniform > 0.);
    REQUIRE(shell_uniform > 0.);
    CHECK(sparse_infill_length(adaptive, 25., 35., core) < max_core * core_uniform);
    CHECK(sparse_infill_length(adaptive, 25., 35., shell) > 0.75 * shell_uniform);
}

TEST_CASE("Adaptive TPMS infill of a tall object is sparsest at its middle height", "[Fill][TpmsAdaptive]")
{
    // A 30 x 30 x 90 mm box in 0.4 mm layers: its center is 45 mm high, the middle of its core, not a column.
    Print print;
    Slic3r::Test::init_and_process_print({make_cube(30., 30., 90.)}, print,
                                        {{"sparse_infill_pattern", "tpmsd"},
                                         {"sparse_infill_density", "25%"},
                                         {"tpms_adaptive", "lobes"},
                                         {"tpms_interior_density", "5%"},
                                         {"tpms_adaptive_gradient", "linear"},
                                         {"layer_height", 0.4},
                                         {"initial_layer_print_height", 0.4}});
    const Polygons core   = centered_square(5.);
    const double   middle = sparse_infill_length(print, 40., 50., core);
    const double   low    = sparse_infill_length(print, 10., 20., core);
    REQUIRE(low > 0.);
    CHECK(middle < 0.7 * low);
}

TEST_CASE("2D adaptive TPMS infill does not change along its axis", "[Fill][TpmsAdaptive]")
{
    // A box of 30 x 30 mm sections, 90 mm long along the axis, centered on the origin in XY.
    const std::string mode = GENERATE("normal_x", "normal_z");
    CAPTURE(mode);
    const bool   along_x = mode == "normal_x";
    auto         slice   = [&](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({along_x ? make_cube(90., 30., 30.) : make_cube(30., 30., 90.)}, print,
                                                         {{"sparse_infill_pattern", "gyroid"},
                                                          {"sparse_infill_density", "25%"},
                                                          {"tpms_adaptive", adaptive},
                                                          {"tpms_interior_density", "5%"},
                                                          {"tpms_adaptive_gradient", "linear"},
                                                          {"layer_height", 0.4},
                                                          {"initial_layer_print_height", 0.4}});
    };
    Print uniform, adaptive;
    slice("disabled", uniform);
    slice(mode, adaptive);

    // The core of the sections near an end and at the middle of the box, at most 5 mm from their center.
    auto rectangle = [](double x0, double x1) {
        return Polygons{Polygon({Point::new_scale(x0, -5.), Point::new_scale(x1, -5.), Point::new_scale(x1, 5.), Point::new_scale(x0, 5.)})};
    };
    auto core = [&](const Print &print, bool end) {
        return along_x ? sparse_infill_length(print, 10., 20., rectangle(end ? -40. : -5., end ? -30. : 5.)) :
                         sparse_infill_length(print, end ? 10. : 40., end ? 20. : 50., centered_square(5.));
    };
    const double end    = core(adaptive, true);
    const double middle = core(adaptive, false);
    REQUIRE(end > 0.);
    CHECK(middle > 0.8 * end);
    CHECK(middle < 1.25 * end);
    CHECK(middle < 0.7 * core(uniform, false));
}

TEST_CASE("Adaptive TPMS shells and blend keep the whole deep core of a tall object sparse", "[Fill][TpmsAdaptive]")
{
    // Distance warp grades it partly along the height, like Lobes, as its warp is centered on the middle.
    const std::string mode = GENERATE("stepped_shells", "smooth_blend");
    CAPTURE(mode);
    // A 30 x 30 x 90 mm box: from 15 to 75 mm high its axis is 15 mm deep, as deep as its center.
    auto slice = [](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({make_cube(30., 30., 90.)}, print,
                                            {{"sparse_infill_pattern", "tpmsd"},
                                             {"sparse_infill_density", "25%"},
                                             {"tpms_adaptive", adaptive},
                                             {"tpms_interior_density", "5%"},
                                             {"tpms_adaptive_gradient", "linear"},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
    };
    Print uniform, distance;
    slice("disabled", uniform);
    slice(mode, distance);
    // At 15 to 25 mm high the axis is as sparse as at the middle, where Lobes grades it towards the bottom.
    const Polygons core        = centered_square(5.);
    const double   uniform_low = sparse_infill_length(uniform, 15., 25., core);
    const double   low         = sparse_infill_length(distance, 15., 25., core);
    const double   middle      = sparse_infill_length(distance, 40., 50., core);
    CAPTURE(uniform_low, low, middle);
    REQUIRE(middle > 0.);
    CHECK(low < 0.7 * uniform_low);
    CHECK(low < 1.25 * middle);
}

TEST_CASE("Adaptive TPMS infill is sparse at the center of both of two united spheres", "[Fill][TpmsAdaptive]")
{
    // Two spheres of 20 mm, their centers 32 mm apart at 20 mm high: centered on the origin, they are at x = -16 and 16.
    TriangleMesh spheres = make_sphere(20., 2. * PI / 90.);
    spheres.translate(-16.f, 0.f, 20.f);
    TriangleMesh second = make_sphere(20., 2. * PI / 90.);
    second.translate(16.f, 0.f, 20.f);
    spheres.merge(second);
    auto slice = [&spheres](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({TriangleMesh(spheres)}, print,
                                            {{"sparse_infill_pattern", "gyroid"},
                                             {"sparse_infill_density", "25%"},
                                             {"tpms_adaptive", adaptive},
                                             {"tpms_interior_density", "5%"},
                                             {"tpms_adaptive_gradient", "linear"},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
    };
    Print uniform, adaptive;
    slice("disabled", uniform);
    slice("lobes", adaptive);
    // Within 5 mm of either center: at most 5% + 20% * 7.1 / 20 = 12.1% dense, where one center for both
    // would leave the other sphere at more than 60% of the uniform density.
    for (const double x : {-16., 16.}) {
        CAPTURE(x);
        Polygons core = centered_square(5.);
        for (Polygon &square : core)
            square.translate(Point::new_scale(x, 0.));
        const double core_uniform = sparse_infill_length(uniform, 18., 22., core);
        REQUIRE(core_uniform > 0.);
        CHECK(sparse_infill_length(adaptive, 18., 22., core) < 0.45 * core_uniform);
    }
}

TEST_CASE("Adaptive TPMS gradients keep the surface density deeper in the order quadratic, linear, exponential", "[Fill][TpmsAdaptive]")
{
    // With a denser surface, t^2 <= t and the geometric interpolation is below the linear one at every depth.
    auto length_for = [](const std::string &gradient) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(40., 40., 40.)}, print,
                                            {{"sparse_infill_pattern", "tpmsd"},
                                             {"sparse_infill_density", "25%"},
                                             {"tpms_adaptive", "lobes"},
                                             {"tpms_interior_density", "5%"},
                                             {"tpms_adaptive_gradient", gradient},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
        return sparse_infill_length(print, 0., 40., centered_square(20.));
    };
    const double quadratic   = length_for("quadratic");
    const double linear      = length_for("linear");
    const double exponential = length_for("exponential");
    CHECK(quadratic > linear);
    CHECK(linear > exponential);
}

TEST_CASE("Adaptive TPMS settings leave the infill unchanged when they do not apply", "[Fill][TpmsAdaptive]")
{
    // Adaptive density turned off, or turned on for a pattern that is no TPMS.
    const auto [pattern, adaptive] = GENERATE(
        table<std::string, std::string>({{"tpmsd", "disabled"}, {"tpmsfk", "disabled"}, {"gyroid", "disabled"}, {"grid", "lobes"}, {"grid", "stepped_shells"}, {"grid", "smooth_blend"}, {"grid", "distance_warp"}, {"grid", "normal_z"}}));
    CAPTURE(pattern, adaptive);
    Print reference, tuned;
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(20., 20., 20.)}, reference,
                                        {{"sparse_infill_pattern", pattern}, {"sparse_infill_density", "20%"}, {"layer_height", 0.2}});
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(20., 20., 20.)}, tuned,
                                        {{"sparse_infill_pattern", pattern},
                                         {"sparse_infill_density", "20%"},
                                         {"layer_height", 0.2},
                                         {"tpms_adaptive", adaptive},
                                         {"tpms_interior_density", "40%"},
                                         {"tpms_adaptive_gradient", "exponential"}});
    const std::vector<Points> expected = sparse_infill_sequence(reference);
    REQUIRE(!expected.empty());
    CHECK(sparse_infill_sequence(tuned) == expected);
}

TEST_CASE("Adaptive gyroid infill ignores the Z-buckling optimization", "[Fill][TpmsAdaptive]")
{
    auto shape_for = [](const std::string &gyroid_optimized) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::make_cube(20., 20., 20.)}, print,
                                            {{"sparse_infill_pattern", "gyroid"},
                                             {"sparse_infill_density", "10%"},
                                             {"gyroid_optimized", gyroid_optimized},
                                             {"tpms_adaptive", "lobes"},
                                             {"layer_height", 0.2}});
        return sparse_infill_sequence(print);
    };
    const std::vector<Points> expected = shape_for("0");
    REQUIRE(!expected.empty());
    CHECK(shape_for("1") == expected);
}

TEST_CASE("Adaptive TPMS infill of a region matches the infill of a larger region of the same object", "[Fill][TpmsAdaptive]")
{
    const InfillPattern pattern = GENERATE(ipGyroid, ipTpmsD, ipTpmsFK);
    CAPTURE(pattern);
    // An 80 x 80 x 40 mm box around both regions, which share its radial field.
    const ExPolygons box{ExPolygon(Points{Point::new_scale(60., 20.), Point::new_scale(140., 20.), Point::new_scale(140., 100.),
                                          Point::new_scale(60., 100.)})};
    std::vector<TpmsRadialField::Slice> slices;
    for (int i = 0; i < 200; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &box});
    const TpmsRadialField field(slices, get_extents(box), TpmsAdaptiveMode::Lobes, [] {});

    auto circle = [](double radius) {
        Polygon contour = make_circle_num_segments(scale_(radius), 120);
        contour.translate(Point::new_scale(100., 60.));
        return ExPolygon(std::move(contour));
    };
    const ExPolygon region = circle(20.);
    const ExPolygon larger = circle(30.);
    auto fill = [pattern, &field](const ExPolygon &expolygon, double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
        filler->spacing           = 0.45;
        filler->angle             = float(M_PI / 7.);
        filler->z                 = z;
        filler->tpms_radial_field = &field;

        FillParams params;
        params.density                = 0.2f;
        params.tpms_adaptive          = TpmsAdaptiveMode::Lobes;
        params.tpms_interior_density  = 0.05f;
        params.tpms_adaptive_gradient = TpmsAdaptiveGradient::Linear;
        params.layer_height           = 0.2;
        params.dont_adjust            = true;
        Surface surface(stInternal, expolygon);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary of the region, where both are clipped and connected the same way.
    const Polygons inner = shrink(to_polygons(region), scale_(1.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };
    // Marching squares simplifies rings that start elsewhere in each region. At 15.325 mm TPMS-FK has a saddle,
    // whose lines connect one way or the other with the sampling grid.
    const double tolerance = SPARSE_INFILL_RESOLUTION + 0.01;
    for (const double z : {5.1, 12.3, 15.325, 20.1, 27.9, 34.7}) {
        CAPTURE(z);
        const Polylines paths = fill(region, z);
        REQUIRE_FALSE(paths.empty());
        const Polylines reference = fill(larger, z);
        CHECK(farthest(reference, paths) < tolerance);
        CHECK(farthest(paths, reference) < tolerance);
    }
}

TEST_CASE("Adaptive TPMS anchors match the printed infill", "[Fill][TpmsAdaptive][InternalBridge]")
{
    const std::string pattern = GENERATE("tpmsd", "tpmsfk", "gyroid");
    CAPTURE(pattern);
    Print print;
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(30., 30., 30.)}, print,
                                        {{"sparse_infill_pattern", pattern},
                                         {"sparse_infill_density", "25%"},
                                         {"tpms_adaptive", "lobes"},
                                         {"tpms_interior_density", "5%"},
                                         {"layer_height", 0.2},
                                         {"initial_layer_print_height", 0.2},
                                         {"resolution", 0.012}});

    const Layer &layer = *print.objects().front()->get_layer(40);
    Polylines printed;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                entity->collect_polylines(printed);
    REQUIRE_FALSE(printed.empty());
    const AABBTreeLines::LinesDistancer<Line> printed_tree(to_lines(printed));

    // Exclude the perimeter connections, which anchoring and extrusion trim differently.
    const Polylines anchors = intersection_pl(layer.generate_sparse_infill_polylines_for_anchoring(nullptr, nullptr, nullptr),
                                              shrink(to_polygons(layer.lslices), scale_(3.)));
    REQUIRE_FALSE(anchors.empty());
    double max_distance = 0.;
    for (const Polyline &path : anchors)
        for (const Point &point : path.equally_spaced_points(scale_(0.25)))
            max_distance = std::max(max_distance, printed_tree.distance_from_lines<false>(point));
    CHECK(unscale<double>(max_distance) <= 0.012);
}
