
//
// pedsim - A microscopic pedestrian simulation system.
// Copyright (c) 2003 - 2014 by Christian Gloor
//
//
// Adapted for Low Level Parallel Programming 2017
//
#include "ped_model.h"
#include "ped_waypoint.h"
#include "ped_model.h"
#include <iostream>
#include <stack>
#include <algorithm>
#include "cuda_testkernel.h"
#include <omp.h>
#include <thread>
#include <emmintrin.h>
#include <stdlib.h>

void Ped::Model::setup(std::vector<Ped::Tagent*> agentsInScenario, std::vector<Twaypoint*> destinationsInScenario, IMPLEMENTATION implementation)
{
	// Convenience test: does CUDA work on this machine?
	cuda_test();
	// Set 
	agents = std::vector<Ped::Tagent*>(agentsInScenario.begin(), agentsInScenario.end());

	X = (int *) _mm_malloc(agents.size() * sizeof(int), 16);
	Y = (int *) _mm_malloc(agents.size() * sizeof(int), 16);
	int i = 0;
	for(auto agent : agents) {
		X[i] = agent->getX();
		Y[i] = agent->getY();
		i++;
	}
	// Set up destinations
	destinations = std::vector<Ped::Twaypoint*>(destinationsInScenario.begin(), destinationsInScenario.end());
	destX = (double *)_mm_malloc(destinations.size() * sizeof(double),16);
	destY = (double *)_mm_malloc(destinations.size() * sizeof(double),16);
	destR = (double *)_mm_malloc(destinations.size() * sizeof(double),16);
	int j = 0;
	for(auto dest : destinations) {
		destX[j] = dest->getx();
		destY[j] = dest->gety();
		destR[j] = dest->getr();
		j++;
		
	}



	// Sets the chosen implemenation. Standard in the given code is SEQ
	this->implementation = implementation;

	// Set up heatmap (relevant for Assignment 4)
	setupHeatmapSeq();
}

void Ped::Model::thread_func(int val, int work){
  for(int i = 0; i < work; i++){
    int index = val + i;
    agents[index]->computeNextDesiredPosition();
    agents[index]->setX(agents[index]->getDesiredX());
    agents[index]->setY(agents[index]->getDesiredY());
  }
}


void Ped::Model::tick()
{
  if(this->implementation == Ped::SEQ){
      for (auto agent: agents){
	agent->computeNextDesiredPosition();
	agent->setX(agent->getDesiredX());
	agent->setY(agent->getDesiredY());
      }
    }
  else if(this->implementation == Ped::OMP){
    int i;
#pragma omp parallel for private(i)
    for (i = 0; i < agents.size(); i++){
      agents[i]->computeNextDesiredPosition();
      agents[i]->setX(agents[i]->getDesiredX());
      agents[i]->setY(agents[i]->getDesiredY());
    } 
  }
  else if(this->implementation == Ped::PTHREAD){
  
     int num_threads = 1;
     if(num_threads > agents.size()){
       num_threads = agents.size();
     }
     std::thread threads[num_threads];
     int num_agents = agents.size();
     int work = num_agents / num_threads;
     int remainder = num_agents % num_threads;
     for(int i = 0; i < num_threads && num_agents; i++){
       int index = i * work;
       threads[i] = std::thread(&Ped::Model::thread_func,this, index, work);
     }
     thread_func(num_agents-remainder,remainder);
     for(int i = 0; i < num_threads; i++){
       threads[i].join();
     }
   }
   else if(this->implementation == Ped::VECTOR){
	   __m128 X,Y,D;
	   for (int i = 0; i < agents.size(); i+4)
	   {
		   X = _mm_load_ps(&X[i]);
	   }
	   
   }
}

////////////
/// Everything below here relevant for Assignment 3.
/// Don't use this for Assignment 1!
///////////////////////////////////////////////

// Moves the agent to the next desired position. If already taken, it will
// be moved to a location close to it.
void Ped::Model::move(Ped::Tagent *agent)
{
	// Search for neighboring agents
	set<const Ped::Tagent *> neighbors = getNeighbors(agent->getX(), agent->getY(), 2);

	// Retrieve their positions
	std::vector<std::pair<int, int> > takenPositions;
	for (std::set<const Ped::Tagent*>::iterator neighborIt = neighbors.begin(); neighborIt != neighbors.end(); ++neighborIt) {
		std::pair<int, int> position((*neighborIt)->getX(), (*neighborIt)->getY());
		takenPositions.push_back(position);
	}

	// Compute the three alternative positions that would bring the agent
	// closer to his desiredPosition, starting with the desiredPosition itself
	std::vector<std::pair<int, int> > prioritizedAlternatives;
	std::pair<int, int> pDesired(agent->getDesiredX(), agent->getDesiredY());
	prioritizedAlternatives.push_back(pDesired);

	int diffX = pDesired.first - agent->getX();
	int diffY = pDesired.second - agent->getY();
	std::pair<int, int> p1, p2;
	if (diffX == 0 || diffY == 0)
	{
		// Agent wants to walk straight to North, South, West or East
		p1 = std::make_pair(pDesired.first + diffY, pDesired.second + diffX);
		p2 = std::make_pair(pDesired.first - diffY, pDesired.second - diffX);
	}
	else {
		// Agent wants to walk diagonally
		p1 = std::make_pair(pDesired.first, agent->getY());
		p2 = std::make_pair(agent->getX(), pDesired.second);
	}
	prioritizedAlternatives.push_back(p1);
	prioritizedAlternatives.push_back(p2);

	// Find the first empty alternative position
	for (std::vector<pair<int, int> >::iterator it = prioritizedAlternatives.begin(); it != prioritizedAlternatives.end(); ++it) {

		// If the current position is not yet taken by any neighbor
		if (std::find(takenPositions.begin(), takenPositions.end(), *it) == takenPositions.end()) {

			// Set the agent's position 
			agent->setX((*it).first);
			agent->setY((*it).second);

			break;
		}
	}
}

/// Returns the list of neighbors within dist of the point x/y. This
/// can be the position of an agent, but it is not limited to this.
/// \date    2012-01-29
/// \return  The list of neighbors
/// \param   x the x coordinate
/// \param   y the y coordinate
/// \param   dist the distance around x/y that will be searched for agents (search field is a square in the current implementation)
set<const Ped::Tagent*> Ped::Model::getNeighbors(int x, int y, int dist) const {

	// create the output list
	// ( It would be better to include only the agents close by, but this programmer is lazy.)	
	return set<const Ped::Tagent*>(agents.begin(), agents.end());
}

void Ped::Model::cleanup() {
	// Nothing to do here right now. 
}

Ped::Model::~Model()
{
	std::for_each(agents.begin(), agents.end(), [](Ped::Tagent *agent){delete agent;});
	std::for_each(destinations.begin(), destinations.end(), [](Ped::Twaypoint *destination){delete destination; });
}
