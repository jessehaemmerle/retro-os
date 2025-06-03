#!/bin/bash

# RetroOS Docker Deployment Script
# This script sets up and runs the RetroOS application using Docker Compose

set -e

# Default environment
ENVIRONMENT="development"
COMPOSE_FILE="docker-compose.yml"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--production)
            ENVIRONMENT="production"
            COMPOSE_FILE="docker-compose.prod.yml"
            shift
            ;;
        -h|--help)
            echo "RetroOS Docker Deployment Script"
            echo ""
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -p, --production    Deploy in production mode"
            echo "  -h, --help         Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                 # Deploy in development mode"
            echo "  $0 --production    # Deploy in production mode"
            exit 0
            ;;
        *)
            echo "Unknown option $1"
            exit 1
            ;;
    esac
done

echo "🚀 Starting RetroOS Docker Deployment in $ENVIRONMENT mode..."

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_info() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    print_error "Docker is not installed. Please install Docker first."
    exit 1
fi

# Check if Docker Compose is installed
if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
    print_error "Docker Compose is not installed. Please install Docker Compose first."
    exit 1
fi

# Create necessary directories
print_status "Creating necessary directories..."
mkdir -p nginx
mkdir -p mongo-init

# Copy environment variables for Docker
print_status "Setting up environment variables..."
if [ -f ".env.docker" ]; then
    cp .env.docker backend/.env.docker
    cp .env.docker frontend/.env.docker
    print_status "Environment files created for Docker deployment"
else
    print_warning ".env.docker not found, using existing .env files"
fi

# Stop any existing containers
print_status "Stopping any existing containers..."
docker-compose -f $COMPOSE_FILE down 2>/dev/null || true

# Setup environment files
if [ "$ENVIRONMENT" = "production" ]; then
    print_status "Setting up production environment..."
    if [ -f "backend/.env.production" ]; then
        cp backend/.env.production backend/.env
    fi
    if [ -f "frontend/.env.production" ]; then
        cp frontend/.env.production frontend/.env
    fi
else
    print_status "Setting up development environment..."
fi

# Build and start the services
print_status "Building and starting services with $COMPOSE_FILE..."
docker-compose -f $COMPOSE_FILE up --build -d

# Wait for services to be ready
print_status "Waiting for services to start..."
sleep 10

# Check service status
print_status "Checking service status..."
docker-compose -f $COMPOSE_FILE ps

# Test the application
print_status "Testing application endpoints..."
sleep 5

# Test backend health
if curl -f http://localhost:8001/api/ > /dev/null 2>&1; then
    print_status "✅ Backend is running successfully"
else
    print_warning "❌ Backend health check failed"
fi

# Test frontend
if curl -f http://localhost:3000 > /dev/null 2>&1; then
    print_status "✅ Frontend is running successfully"
else
    print_warning "❌ Frontend health check failed"
fi

# Test nginx proxy
if curl -f http://localhost/ > /dev/null 2>&1; then
    print_status "✅ Nginx proxy is running successfully"
else
    print_warning "❌ Nginx proxy health check failed"
fi

echo ""
print_status "🎉 Deployment completed!"
echo ""
echo "📱 Application URLs:"
echo "   • Main Application: http://localhost"
echo "   • Frontend Only:    http://localhost:3000"
echo "   • Backend API:      http://localhost:8001/api"
echo ""
echo "🔧 Management Commands:"
echo "   • View logs:        docker-compose logs -f"
echo "   • Stop services:    docker-compose down"
echo "   • Restart:          docker-compose restart"
echo "   • Rebuild:          docker-compose up --build -d"
echo ""
echo "📊 MongoDB Connection:"
echo "   • Host: localhost"
echo "   • Port: 27017"
echo "   • Database: retroos"
echo ""

# Show running containers
print_status "Running containers:"
docker-compose ps