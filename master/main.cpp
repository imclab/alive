#include "main.h"
#include "Q.hpp"
#include "gigaverb.h"

#include "allocore/graphics/al_DisplayList.hpp"
#include "allocore/spatial/al_HashSpace.hpp"
#include "alloutil/al_OmniApp.hpp"
#include "alloutil/al_Lua.hpp"

#include "al_SharePOD.hpp"

#include <stdio.h>
#include <stdlib.h>

using namespace al;

bool bMaster = true;
bool bRemoteAudio = false;

// audio globals:
double samplerate = 44100;
double invsamplerate = 1./samplerate;
double doppler_scale = (DOPPLER_SAMPLES - 512) / double(WORLD_DIM);

const uint32_t WAVEBITS = 10;
const uint32_t WAVESIZE = 1 << WAVEBITS;
const uint32_t FRACBITS = 32 - WAVEBITS;
const uint32_t FRACMASK = ( 1 << FRACBITS ) - 1;
const double FRACSCALE = 1.0 / ( 1 << FRACBITS );


// extra sample for linear interp:
double sine_wavetable [ WAVESIZE + 1 ];

void init_wavetables() {
	for (uint32_t i=0; i<=WAVESIZE+1; i++) {
		double p = double(i) / WAVESIZE;
		sine_wavetable[i] = sin(M_PI * 2. * p);
	}
}

void default_synthesize_func(struct Voice& self, int frames, float * out) {
	const uint32_t om = uint32_t(self.freq * invsamplerate * 4294967296.0); // 2^32
	uint32_t p = self.iphase;
	for (int i=0; i<frames; i++) {
		const uint32_t idx = p >> FRACBITS;
		const double s0 = sine_wavetable[idx];
		const double s1 = sine_wavetable[idx+1];
		const double a = (p & FRACMASK) * FRACSCALE;
		out[i] = s0 + a*(s1 - s0);
		p += om;
	}
	self.iphase = p;
}

inline vec3 quat_unrotate(const quat& q, const vec3& v) {
    // reduced:
	double px = q.w*v.x - q.y*v.z + q.z*v.y;
    double py = q.w*v.y - q.z*v.x + q.x*v.z;
    double pz = q.w*v.z - q.x*v.y + q.y*v.x;
	double pw = q.x*v.x + q.y*v.y + q.z*v.z;
    return vec3(
        pw*q.x + px*q.w + py*q.z - pz*q.y,  // x
        pw*q.y + py*q.w + pz*q.x - px*q.z,  // y
        pw*q.z + pz*q.w + px*q.y - py*q.x   // z
    );
}

Q<audiomsg_packet> audioq;
double audiotime = 0;	// seconds
double maintime = 0;	// seconds
double audiolag = 0.04; // seconds

audiomsg * audioq_head() {
	return (audiomsg *)audioq.head();
}
void audioq_send() {
	audioq.q[audioq.write].t = audiotime + audiolag;
	audioq.send();
}

audiomsg * audioq_peek(double maxtime) {
	audiomsg_packet * p = audioq.peek();
	return (p && p->t < maxtime) ? (audiomsg *)p : 0;
}
audiomsg * audioq_next(double maxtime) {
	audioq.next();
	return audioq_peek(maxtime);
}

#pragma mark App

class App;

App * app = 0;

class App : public Global, public OmniApp, public SharedBlob::Handler {
public:

	App() 
	:	OmniApp("alive", !bMaster),
		space(5, MAX_AGENTS),
		qnearest(6)
	{
		app = this;
		
		title("alive");
		
		// one-time only:
		if (omni().activeStereo()) {
			omni().resolution(2048);
			bRemoteAudio = true;
		} else {
			omni().resolution(256);
		}
		lens().fovy(85);
		lens().far(WORLD_DIM * 0.5);
		
		mNavSpeed = 0.1;
		
		nav().halt();
		nav().home();
		
		printf("running on %s\n", hostName().c_str());
		printf("blob %d\n", (int)sizeof(Shared));
		
		updating = true;
		
		// initialize shared:
		shared.framecount = 0;
		shared.mode = 0;
		shared.bgcolor.set(0.25);
		shared.eyesep = 0.03;
		shared.show_collisions = 0;
		
		reset();	
		
		// start sharing:
		if (bMaster) {
		
			if (hostName() == "photon") {
				AudioDevice::printAll();
				AudioDevice indev("system", AudioDevice::INPUT);
				AudioDevice outdev("system", AudioDevice::OUTPUT);
				//initAudio("system", 44100, 1024, 12, 12);
				
				audioIO().deviceIn(indev);
				audioIO().deviceOut(outdev);
				audioIO().channelsOut(12);
				initAudio(44100, 1024);
				
				audioIO().print();
				
			} else {
				initAudio(44100, 1024);
			}

			initAudio(44100, 1024);
			audiotime = 0;
			doppler_strength = 1.;
			
			audiogain = 0.5;
			reverbgain = 0.03;
			
			verb.reset();
			
			update = 0;
			
			// we want some extra space for e.g. reverb channels:
			audioIO().channelsBus(2);
			
			// stereo mode:
			numActiveSpeakers = 2;
			for (int i=0; i<numActiveSpeakers; i++) {
				SpeakerConfig& s = speakers[i];
				double angle = M_PI * 0.5 * (i - 0.5);
				s.weights.w = M_SQRT1_2;
				s.weights.x = sin(angle);
				s.weights.y = 0.;
				s.weights.z = -cos(angle);
			}
		
			pod.startServer(this);
			
		} else {
			pod.startClient(this, "photon");
		}
	}
	
	void reset() {
		for (int i=0; i<MAX_AGENTS; i++) {
			Agent& a = shared.agents[i];
			a.id = i;
			
			Voice& v = voices[i];
			v.id = i;
			
			agent_reset(a);
			
		}
	}
	
	virtual std::string	vertexCode() {
		return AL_STRINGIFY(
			// omniapp also adds these uniforms:
			// uniform float omni_eye;
			// uniform int omni_face;	
			// uniform float omni_near;	
			// uniform float omni_far;
		
			attribute vec4 rotate;
			attribute vec3 translate;
			attribute vec3 scale;
			varying vec4 color;
			varying vec3 normal, lightDir, eyeVec;
			varying vec4 La, Ld, Ls;
			varying float fog;
			
			//	q must be a normalized quaternion
			vec3 quat_rotate(vec4 q, vec3 v) {
				// qv = vec4(v, 0) // 'pure quaternion' derived from vector
				// return ((q * qv) * q^-1).xyz
				// reduced to 24 multiplies and 17 additions:
				vec4 p = vec4(
					q.w*v.x + q.y*v.z - q.z*v.y,	// x
					q.w*v.y + q.z*v.x - q.x*v.z,	// y
					q.w*v.z + q.x*v.y - q.y*v.x,	// z
					-q.x*v.x - q.y*v.y - q.z*v.z	// w
				);
				return vec3(
					p.x*q.w - p.w*q.x + p.z*q.y - p.y*q.z,	// x
					p.y*q.w - p.w*q.y + p.x*q.z - p.z*q.x,	// y
					p.z*q.w - p.w*q.z + p.y*q.x - p.x*q.y	// z
				);
			}
			
			void main(){
				vec3 P = translate + quat_rotate(rotate, gl_Vertex.xyz * scale);
				vec4 vertex = gl_ModelViewMatrix * vec4(P, gl_Vertex.w);
				
				// fog calculation:
				//float distance = length(vertex.xyz);
				// because our world is a bounded cube:
				float distance = max(abs(vertex.x), max(abs(vertex.y), abs(vertex.z)));
				float fogstart = omni_far * 0.65;
				// distance over fog range clamped to 0,1:
				float unitdistance = clamp((distance - fogstart - omni_near) / (omni_far - fogstart - omni_near), 0., 1.);
				// cheap curved fog effect:
				fog = unitdistance * unitdistance;
				
				gl_Position = omni_render(vertex); 
				
				normal = gl_NormalMatrix * gl_Normal;
				color = gl_Color;
				
				vec3 V = vertex.xyz;
				eyeVec = normalize(-V);
				lightDir = normalize(vec3(gl_LightSource[0].position.xyz - V));
				La = gl_LightSource[0].ambient;
				Ld = gl_LightSource[0].diffuse;
				Ls = gl_LightSource[0].specular;
			}
		);
	}

	virtual std::string fragmentCode() {
		return AL_STRINGIFY(
			uniform float lighting;
			uniform vec4 bgcolor;
			varying vec4 color;
			varying vec3 normal, lightDir, eyeVec;
			varying vec4 La, Ld, Ls;
			varying float fog;
			void main() {
				vec4 final_color = color * La;
				vec3 N = normalize(normal);
				vec3 L = lightDir;
				float lambertTerm = max(dot(N, L), 0.0);
				final_color += Ld * color * lambertTerm;
				vec3 E = eyeVec;
				vec3 R = reflect(-L, N);
				float spec = max(dot(R, E), 0.0);
				//spec = pow(spec, 1.);
				final_color += Ls * spec;
				final_color = mix(color, final_color, lighting);
				gl_FragColor = mix(final_color, bgcolor, fog);
			}
		);
	}
	
	virtual void onDraw(Graphics& gl) {
		shader().uniform("lighting", 0.2);
		shader().uniform("bgcolor", shared.bgcolor.r, shared.bgcolor.g, shared.bgcolor.b, shared.bgcolor.a);
		
		//gl.polygonMode(gl.LINE);
		
		// draw all active agents
		int translateAttr = shader().attribute("translate");
		int rotateAttr = shader().attribute("rotate");
		int scaleAttr = shader().attribute("scale");
		for (int i=0; i<MAX_AGENTS; i++) {
			Agent& a = shared.agents[i];
			if (a.enable && a.visible) {
				shader().attribute(rotateAttr, a.rotate.x, a.rotate.y, a.rotate.z, a.rotate.w);
				shader().attribute(translateAttr, a.position.x, a.position.y, a.position.z);
				shader().attribute(scaleAttr, a.scale.x, a.scale.y, a.scale.z);
				gl.color(a.color.r, a.color.g, a.color.b, a.color.a);
				cubelist.draw();
				
				// now draw tails:
				
				//a.trail_start should be the most recent one
				//a.trail_start - 1 would be the prev
				int start = a.trail_start;
				int size = a.trail_size;
				double fade = 1;
				for (int i = 1; i < size - 1; i++) {
					int idx = start + i;
					const Trail& trail = a.trails[idx % (TRAIL_LENGTH - 1)];
					
					// use 0.9 if TRAIL_LENGTH is 16.
					fade *= 0.8; // fades to 0.16 after 8 times
					
					shader().attribute(rotateAttr, trail.rotate.x, trail.rotate.y, trail.rotate.z, trail.rotate.w);
					shader().attribute(translateAttr, trail.position.x, trail.position.y, trail.position.z);
					shader().attribute(scaleAttr, a.scale.x * fade, a.scale.y * fade, a.scale.z);
					cubelist.draw();
				}
			}
		}
		
		if (shared.show_collisions) {
			shader().attribute(rotateAttr, 0, 0, 0, 1);
			shader().attribute(translateAttr, 0, 0, 0);
			shader().attribute(scaleAttr, 1, 1, 1);
			gl.color(0.2);
			gl.begin(gl.LINES);
			for (int i=0; i<MAX_AGENTS; i++) {
				Agent& a = shared.agents[i];
				
				if (a.enable && a.visible) {
					Agent& b = shared.agents[a.nearest];
					gl.vertex(a.position);
					gl.vertex(b.position);
				}
			}
			gl.end();
		}
	}
	
	void simulate(al_sec dt) {
		// call back into LuaJIT:
		if (update) update(*this, dt);
		
		for (int i=0; i<MAX_AGENTS; i++) {
			Agent& a = shared.agents[i];
			if (a.enable) {
				
				// update unit vectors:
				a.rotate.toVectorX(a.ux);
				a.rotate.toVectorY(a.uy);
				a.rotate.toVectorZ(a.uz);
				
				// update hashspace:
				space.move(i, 
					a.position.x,
					a.position.y,
					a.position.z
				);

				HashSpace::Object& o =  space.object(i);
				HashSpace::Object * n = qnearest.nearest(space, &o);
				if (n) {
					a.nearest = n->id;	
					a.nearest_distance = qnearest.distance(0);
				} else {
					a.nearest = a.id;
					a.nearest_distance = 0;
				}

				// add to trail:
				a.trail_start = (a.trail_start - 1) & (TRAIL_LENGTH - 1);
				if (a.trail_size < TRAIL_LENGTH) a.trail_size++;
				a.trails[a.trail_start].position = a.position;
				a.trails[a.trail_start].rotate = a.rotate;
			}
		}
	}
	
	virtual void onAnimate(al_sec dt) {
		omni().clearColor() = shared.bgcolor;
		lens().eyeSep(shared.eyesep);
	
		// on create:
		if (frame == 1) {
			// allocate GPU resources:
			cube.primitive(gl.QUADS);
			addCube(cube, true, 0.5);
			// shift it forward:
			cube.translate(0, 0, -0.25);
			
			cubelist.begin();
			gl.draw(cube);
			cubelist.end();
			
			shader().begin();
			shader().uniform("lighting", 0.8);
			shader().end();
		}
		
		if (bMaster) {
			shared.framecount++;
			
			// nav updated:
			for (int j=0; j<3; j++) {
				shared.active_origin[j] = floor(nav().pos()[j] - WORLD_DIM/2);
			}
			
			if (updating) simulate(dt);
		} else {
			pod.clientRequest();
		}

		
		//printf("audio cpu %f\n", audioIO().cpu());
	}
	
	virtual void onSound(AudioIOData& io) {
		// only master makes sound:
		if (!bMaster) return;
	
		int frames = io.framesPerBuffer();
		samplerate = io.framesPerSecond();
		invsamplerate = 1./samplerate;
		
		double dt = frames * invsamplerate;
		
		if (audiotime == 0) {
			printf("audio started (remote == %d) %d samples, %f Hz, %dx%d + %d\n", bRemoteAudio, frames, samplerate, io.channelsIn(), io.channelsOut(), io.channelsBus());
		}
		io.zeroOut();
		io.zeroBus();
		
		float * R = io.busBuffer(0); // the reverb bus
		
		// the raw decode outs
		float * W = io.outBuffer(0);
		float * X = io.outBuffer(1);
		float * Y = io.outBuffer(2);
		float * Z = io.outBuffer(3);
		
		// desktop stereo mode:
		float * out0 = io.outBuffer(0);
		float * out1 = io.outBuffer(1);
		
		vec4 w0 = speakers[0].weights;
		vec4 w1 = speakers[1].weights;
		Pose& view = nav();
		
		// process incoming messages:
		double nexttime = audiotime + frames / samplerate;
		
		// play all agents:
		for (int i=0; i<MAX_AGENTS; i++) {
			Agent& a = shared.agents[i];
			if (a.enable) {
			
				// do movement here in audio thread:
				if (updating) {
					
					// accumulate velocity:
					vec3 vel = a.acceleration + a.uz * -a.velocity;
					a.position += vel * dt;
					
					// accumulate rotation:
					vec3 turn = a.twist + a.turn * dt;
					quat r = quat().fromEuler(turn.y, turn.x, turn.z);
					
					// apply rotation:
					a.rotate = a.rotate * r;
					a.rotate.normalize();
					
					// kill acceleration:
					a.twist.set(0);
					a.acceleration = 0;
				}
				
				// wrap location:
				for (int j=0; j<3; j++) {
					double p = a.position[j];
					p -= shared.active_origin[j];
					p = al::wrap(p, (double)WORLD_DIM);
					p += shared.active_origin[j];
					a.position[j] = p;
				}
				
				// now synthesize:
				
				// get position in 'view space':
				vec3 rel = quat_unrotate(view.quat(), a.position - view.pos());
				// distance squared:
				double d2 = rel.dot(rel);
				// distance
				double d = sqrt(d2);
				// unit rel:
				vec3 direction = rel * (1./d);			
				// amplitude scale by distance:
				double atten = attenuate(d2, 0.2, 0.04);
				// omni mix is also distance-dependent. 
				// at near distances, the signal should be omnidirectional
				// the minimum really depends on the radii of the listener/emitter
				double spatial = 1. - attenuate(d2, 0.1, 0.9);
				// encode matrix:
				// first 3 harmonics are the same as the unit direction:
				vec4 encode(
					atten * spatial * direction.x,
					atten * spatial * direction.y,
					atten * spatial * direction.z,
					atten // * M_SQRT2
				);
				
				// render into doppler buffer:
				Voice& v = voices[i];
				(v.synthesize)(v, frames, v.buffer + v.buffer_index);
				
				// decode:
				double invframes = 1./frames;
				for (int j=0; j<frames; j++) { 
					
					// linear interpolate encoding matrix:
					double alpha = j * invframes;
					vec4 enc = ipl::linear(alpha, v.encode, encode);
					double dist = linear_interp(alpha, v.distance, d);
					
					// doppler lookup
					// take current buffer index
					// shift backwards by distance-dependent doppler time
					double idx = (v.buffer_index + j) + (DOPPLER_SAMPLES - dist * doppler_scale * doppler_strength);
					int32_t idx1 = int32_t(idx);
					double idxf = idx - double(idx1); // will this work?
					
					float s0 = v.buffer[(idx1 - 1) & (DOPPLER_SAMPLES - 1)];
					float s1 = v.buffer[idx1 & (DOPPLER_SAMPLES - 1)];
					float s = linear_interp(idxf, s0, s1);
					
						
					if (j==0) {
						//printf("agent %d: d2 %f atten %f spatial %f w %f\n", i, d2, atten, spatial, w0.w);
						//printf("idx %f idx0 %d idx1 %d fract %f\n",  idx, idx0, idx1, idxf);
					}
					
					s *= v.amp * audiogain;
					
					R[j] += s * reverbgain;
						
					// allosphere doesn't decode:
					if (bRemoteAudio) {
					
						// don't decode locally:
						W[j] += s; // * enc.w;
						X[j] += s * enc.x;
						Y[j] += s * enc.y;
						Z[j] += s * enc.z;
						
					} else {
					
						// local decode (stereo):
						out0[j] = out0[j] + s * (
							+ w0.x * enc.x
							+ w0.y * enc.y 
							+ w0.z * enc.z
							+		 enc.w	//  * w0.w
						);
						
						out1[j] = out1[j] + s * (
							+ w1.x * enc.x
							+ w1.y * enc.y 
							+ w1.z * enc.z
							+		 enc.w	//  * w1.w
						);
					}
				}
				
				// update cached:
				v.direction = direction;
				v.distance = d;
				v.encode = encode;
				v.buffer_index = (v.buffer_index + frames) & (DOPPLER_SAMPLES - 1);
			}
		}
			
		// do the reverb:
		if (bRemoteAudio) {
			verb.perform(R, X, Y, frames);
		} else {
			verb.perform(R, out0, out1, frames);
		}
		
		audiotime = nexttime;	
	}
	
	// this handler is called when a client receives blob from the server
	virtual void onReceivedSharedBlob(const char * blob, size_t size) {
		uint32_t framecount = ((Shared *)blob)->framecount;
		//if (framecount % 50 == 0) printf("sent %d\n", framecount);
		memcpy(&shared, blob, size);
	}
	
	// this handler is called when a server requires data to send to a client
	virtual char * onSendSharedBlob() {
		//if (shared.framecount % 50 == 0) printf("sent %d\n", shared.framecount);
		return (char *)&shared;
	}
	
	virtual bool onKeyDown(const Keyboard& k) {
		switch (k.key()) {
			case Keyboard::TAB:
				if (shared.eyesep == 0) {
					shared.eyesep = 0.03;
				} else {
					shared.eyesep = 0.;
				}
				break;
			case 32: // space
				updating = !updating;
				break;
			case 8: // backspace
				nav().halt();
				nav().home();
				break;
			case 'H':
				sendHandshake();
				break;
			case 'O':
				omniEnable(!omniEnable());
				break;
			default:
				printf("key down %d\n", k.key());
				break;
		}
		return 1;
	}
	
	virtual void onMessage(osc::Message& m) {
		OmniApp::onMessage(m);
		
	}
	
	Lua L;
	HashSpace space;
	// this query will consider 6 matches and return the best.
	HashSpace::Query qnearest;
	
	Graphics gl;
	Mesh cube;
	DisplayList cubelist;
	
	SharePOD<Shared> pod;
	
	Gigaverb verb;
	
	bool updating;
};

Global * global_get() {
	return (Global *)app;
}

void agent_reset(Agent& a) {
	a.nearest = a.id;
	a.nearest_distance = 0;
	a.enable = 0;
	a.visible = 1;
			
	// trails:
	a.trail_start = 0;
	a.trail_size = 0;
	
	a.position.x = WORLD_DIM * rnd::global().uniform();
	a.position.y = WORLD_DIM * rnd::global().uniform();
	a.position.z = WORLD_DIM * rnd::global().uniform();
	
	a.velocity = 0;
	a.acceleration = 0;
	a.turn.set(0);
	a.twist.set(0);
	a.color.set(0.5);
	a.scale.set(0.25, 0.125, 0.5);
	
	// init unit vectors:
	a.rotate.toVectorX(a.ux);
	a.rotate.toVectorY(a.uy);
	a.rotate.toVectorZ(a.uz);
	
	Voice& v = global_get()->voices[a.id];
	
	memset(v.buffer, 0, sizeof(float) * DOPPLER_SAMPLES);
	v.encode.set(0, 0, 0, 0);
	v.direction.set(0, 0, 0);
	v.distance = WORLD_DIM;
	v.buffer_index = 0;
	v.iphase = 0;
	v.amp = 0.1;
	v.freq = rnd::global().uniform() * rnd::global().uniform() * 4000;
	v.phase = 0;
	v.synthesize = default_synthesize_func;
}	

int main(int argc, char * argv[]) {
	
	std::string hostName = Socket::hostName();
	std::string masterName;
	if (argc > 1) {
		masterName = argv[1];
	} else {
		masterName = hostName;
	}

	if (masterName == hostName) {
		printf("I AM THE MASTER\n");
		bMaster = true;
	} else {
		printf("I AM NOT THE MASTER\n");
		bMaster = false;
	}

	init_wavetables();
	
	app = new App;
	
	// run main script:
	app->L.dofile("main.lua");
	
	app->start();
	return 0;
}